#pragma once

// Character-part census, arm hide, hand-band classifier and controller
// hand markers.
//
// Part census (developer build; two BeginScene
// passes per tick): logs every world-space draw whose world matrix (or,
// for user-pointer draws, whose first transformed vertex) is within
// PART_RADIUS of the player's feet: call type, FVF, primitive counts,
// stage-0 texture, world translation, the game's return address and the
// local bounding box.
// The whole body, arms included, draws under one root matrix at the feet,
// so arms are told apart per draw, not per matrix.
//
// Hand markers (developer build, [vr] hand_marker / aim_marker): an
// octahedron at each controller's grip pose, stretched along -z, blue =
// left, red = right, dimmed while the pose is extrapolated rather than
// tracked. Checks the whole chain tracking -> world -> per-eye rendering.
// Drawn once per pass after the first scene draw, z-tested, gameplay only.

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "psobbvr_eyeheight.hpp"
#include "psobbvr_gamecam.hpp"
#include "psobbvr_stereo.hpp"
#include "psobbvr_trim.hpp"
#include "psobbvr_vr.hpp"
#include "psobbvr_weapongrip.hpp"

namespace partmap {


inline bool feet_valid = false;
inline float feet[3] = {};

// The game's most recent WORLD matrix (and, for the census, whether it
// sits near the player).
inline D3DMATRIX current_world = {};
inline bool current_world_valid = false;

// ---- arm hide --------------------------------------------------------------
// The body run: consecutive draws issued while the WORLD matrix is the
// player's body root. A run opens on a set that:
//  - comes from the character-model matrix-set site 0x844AFD (body and
//    head cluster; drop shadow uses 0x844B50, equipment 0x82F1FF, scenery
//    0x848F0D);
//  - is within 3.0 units horizontally of the feet read live at set time
//    (attack lunges move the root up to ~2.4 from frame-start feet);
//  - is within +/-0.5 units vertically.
// Re-sets of the same translation continue the run; any other closes it.
// Draws 1..hide_arms_count of the run are the arms and hands (the order is
// stable). Another character's root right at our feet would get its first
// draws hidden too; collision keeps roots apart in practice.
constexpr uintptr_t BODY_ROOT_W_SET_SITE = 0x844AFD;
inline bool body_run_open = false;
inline float body_run_t[3] = {};
inline int body_run_draws = 0;
// The entity's feet, read at matrix-set time (pointer resolved once per
// pass in OnFrame).
inline const float* live_feet = nullptr;

// boxed: the minimap radar draws each player arrow from this same site at
// the player's x/z and height 0 (0x8034E4's arrow loop), so on a floor at
// height 0 our own arrow would open a run and be hidden.
inline void OnWorldTransformArmHide(const D3DMATRIX& m, void* ret, bool boxed) {
    if (boxed) {
        body_run_open = false;
        return;
    }
    if (body_run_open &&
        (fabsf(m._41 - body_run_t[0]) > 0.01f ||
         fabsf(m._42 - body_run_t[1]) > 0.01f ||
         fabsf(m._43 - body_run_t[2]) > 0.01f))
        body_run_open = false;
    if (!body_run_open && live_feet != nullptr &&
        reinterpret_cast<uintptr_t>(ret) == BODY_ROOT_W_SET_SITE) {
        const float dx = m._41 - live_feet[0];
        const float dyv = m._42 - live_feet[1];
        const float dz = m._43 - live_feet[2];
        if (dyv > -0.5f && dyv < 0.5f && dx * dx + dz * dz < 9.0f) {
            body_run_open = true;
            body_run_t[0] = m._41;
            body_run_t[1] = m._42;
            body_run_t[2] = m._43;
            body_run_draws = 0;
        }
    }
}

// Whether to skip the current world-space draw. Counts every body-run draw
// so the indexes stay aligned while hiding is off.
inline bool HideArmDrawNow() {
    if (!body_run_open)
        return false;
    body_run_draws++;
    return vrmod::config.hide_arms && gamecam::DrivesView() &&
           body_run_draws <= vrmod::config.hide_arms_count;
}


// ---- hand-band classifier --------------------------------------------------
// Classifies the body run's draws by whether they track the hand bone, to
// find the right-hand draws. Each body-run user-pointer draw contributes
// its vertex centroid's distance to the hand bones
// (vrmod::hand_bone_world_*), its motion between passes and its
// root-local position. At the end: one line per draw index plus the
// contiguous runs under HANDBAND_NEAR. Only passes where the body drew
// count. Also run automatically by the hands module (quiet).
constexpr int HANDBAND_MAX_DRAWS = 200;
constexpr float HANDBAND_NEAR = 3.0f;  // right-band suggestion cutoff, game units

struct HandBandStat {
    int n;                 // drawing passes this index appeared in
    int n_dist;            // of those, right hand bone was valid
    int n_dist_left;       // left bone valid (duals only)
    int n_move;            // pass-pairs contributing a motion delta
    float dist_sum, dist_min, dist_max;
    float dist_left_sum;
    float move_sum;        // world centroid delta between observed passes
    float local_sum[3];    // body-root-local centroid (vertices are root-local)
    float last_w[3];       // world centroid last pass (for the motion delta)
    bool have_last;
    UINT verts;            // vertex count (stable per index)
};
inline HandBandStat handband_stats[HANDBAND_MAX_DRAWS] = {};
inline int handband_passes = 0;       // countdown, body-drawing passes
inline int handband_seen_passes = 0;  // passes with >= 1 body-run draw
inline int handband_bone_passes = 0;  // of those, right bone valid
inline int handband_max_index = 0;
inline bool handband_drew_last_pass = false;
// The best right-hand run for the hands module: lowest mean bone distance
// among runs of >= 4 draws (else >= 2). handband_done is set at the report
// and cleared by the consumer; handband_quiet skips the per-index lines.
inline bool handband_quiet = false;
inline bool handband_done = false;
inline int handband_best_first = 0, handband_best_last = 0, handband_best_n = 0;
inline float handband_best_mean = 0.0f;

// Mean bone distance of one body-draw index from the last window (-1 if
// unobserved); the hands module trims the band ends with it.
inline float HandBandMean(int i) {
    if (i <= 0 || i >= HANDBAND_MAX_DRAWS || handband_stats[i].n_dist == 0)
        return -1.0f;
    return handband_stats[i].dist_sum / handband_stats[i].n_dist;
}

inline void HandBandReport() {
    handband_best_first = handband_best_last = handband_best_n = 0;
    handband_best_mean = 0.0f;
    diag::Log("handband: done - %d body passes (right bone valid in %d), max index d%d",
              handband_seen_passes, handband_bone_passes, handband_max_index);
    for (int i = 1; i <= handband_max_index && i < HANDBAND_MAX_DRAWS && !handband_quiet; i++) {
        const HandBandStat& s = handband_stats[i];
        if (s.n == 0)
            continue;
        const float move = s.n_move > 0 ? s.move_sum / s.n_move : 0.0f;
        const float lx = s.local_sum[0] / s.n;
        const float ly = s.local_sum[1] / s.n;
        const float lz = s.local_sum[2] / s.n;
        if (s.n_dist > 0) {
            char left[40] = "";
            if (s.n_dist_left > 0)
                _snprintf_s(left, sizeof(left), _TRUNCATE, " distL=%.2f",
                            s.dist_left_sum / s.n_dist_left);
            diag::Log("handband: d%d n=%d v=%u distR mean=%.2f min=%.2f max=%.2f%s move=%.2f local=(%+.1f,%.1f,%+.1f)",
                      i, s.n, s.verts, s.dist_sum / s.n_dist, s.dist_min,
                      s.dist_max, left, move, lx, ly, lz);
        } else {
            diag::Log("handband: d%d n=%d v=%u distR n/a move=%.2f local=(%+.1f,%.1f,%+.1f)",
                      i, s.n, s.verts, move, lx, ly, lz);
        }
    }
    // Runs of indices whose mean distance to the right hand bone stays
    // under the cutoff. (With a single weapon there is no left bone.)
    int run_start = 0;
    float run_dist_sum = 0.0f;
    int run_n = 0;
    for (int i = 1; i <= handband_max_index + 1; i++) {
        const bool in = i <= handband_max_index && i < HANDBAND_MAX_DRAWS &&
                        handband_stats[i].n > 0 && handband_stats[i].n_dist > 0 &&
                        handband_stats[i].dist_sum / handband_stats[i].n_dist <
                            HANDBAND_NEAR;
        if (in) {
            if (run_start == 0) {
                run_start = i;
                run_dist_sum = 0.0f;
                run_n = 0;
            }
            run_dist_sum +=
                handband_stats[i].dist_sum / handband_stats[i].n_dist;
            run_n++;
        } else if (run_start != 0) {
            if (run_n >= 2) {
                const float mean = run_dist_sum / run_n;
                diag::Log("handband: SUGGEST right-hand band d%d..d%d (%d draws, mean bone dist %.2f)",
                          run_start, i - 1, run_n, mean);
                // Lowest mean among >= 4-draw runs; a shorter run only if
                // nothing longer exists.
                const bool better =
                    handband_best_n == 0 ||
                    (run_n >= 4 && handband_best_n < 4) ||
                    ((run_n >= 4) == (handband_best_n >= 4) && mean < handband_best_mean);
                if (better) {
                    handband_best_first = run_start;
                    handband_best_last = i - 1;
                    handband_best_n = run_n;
                    handband_best_mean = mean;
                }
            }
            run_start = 0;
        }
    }
    handband_done = true;
}

// One body-run user-pointer draw while the classifier is armed. Must be
// called right before HideArmDrawNow(), which gives this draw index
// body_run_draws+1 (hidden draws are observed too).
inline void HandBandObserve(const void* data, UINT stride, UINT verts) {
    if (handband_passes <= 0 || !body_run_open || data == nullptr ||
        stride < 12 || verts == 0)
        return;
    const int idx = body_run_draws + 1;
    if (idx >= HANDBAND_MAX_DRAWS)
        return;
    if (idx == 1) {
        handband_seen_passes++;
        if (vrmod::have_hand_bone_right)
            handband_bone_passes++;
        handband_drew_last_pass = true;
    }
    if (idx > handband_max_index)
        handband_max_index = idx;

    // Root-local centroid of the positions (first 3 floats per vertex).
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
    float c[3] = {};
    for (UINT i = 0; i < verts; i++, p += stride) {
        const float* v = reinterpret_cast<const float*>(p);
        c[0] += v[0];
        c[1] += v[1];
        c[2] += v[2];
    }
    c[0] /= verts;
    c[1] /= verts;
    c[2] /= verts;
    // World centroid (current_world is the root while the run is open).
    const D3DMATRIX& w = current_world;
    const float wx = c[0] * w._11 + c[1] * w._21 + c[2] * w._31 + w._41;
    const float wy = c[0] * w._12 + c[1] * w._22 + c[2] * w._32 + w._42;
    const float wz = c[0] * w._13 + c[1] * w._23 + c[2] * w._33 + w._43;

    HandBandStat& s = handband_stats[idx];
    s.n++;
    s.verts = verts;
    s.local_sum[0] += c[0];
    s.local_sum[1] += c[1];
    s.local_sum[2] += c[2];
    if (vrmod::have_hand_bone_right) {
        const float dx = wx - vrmod::hand_bone_world_right._41;
        const float dy = wy - vrmod::hand_bone_world_right._42;
        const float dz = wz - vrmod::hand_bone_world_right._43;
        const float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (s.n_dist == 0 || d < s.dist_min)
            s.dist_min = d;
        if (d > s.dist_max)
            s.dist_max = d;
        s.dist_sum += d;
        s.n_dist++;
    }
    if (vrmod::have_hand_bone_left) {
        const float dx = wx - vrmod::hand_bone_world_left._41;
        const float dy = wy - vrmod::hand_bone_world_left._42;
        const float dz = wz - vrmod::hand_bone_world_left._43;
        s.dist_left_sum += sqrtf(dx * dx + dy * dy + dz * dz);
        s.n_dist_left++;
    }
    if (s.have_last) {
        const float dx = wx - s.last_w[0];
        const float dy = wy - s.last_w[1];
        const float dz = wz - s.last_w[2];
        s.move_sum += sqrtf(dx * dx + dy * dy + dz * dz);
        s.n_move++;
    }
    s.last_w[0] = wx;
    s.last_w[1] = wy;
    s.last_w[2] = wz;
    s.have_last = true;
}


// Once per BeginScene pass (two per game tick).
inline void OnFrame() {
    current_world_valid = false;
    body_run_open = false;
    body_run_draws = 0;


    feet_valid = false;
    live_feet = nullptr;
    const uintptr_t entity = gamecam::ResolveEntity();
    if (entity != 0) {
        const float* p = reinterpret_cast<const float*>(entity + gamecam::ENTITY_POS_OFFSET);
        feet[0] = p[0];
        feet[1] = p[1];
        feet[2] = p[2];
        feet_valid = true;
        live_feet = p;  // arm hide re-reads at matrix-set time
    }

    // Eye height (psobbvr_eyeheight.hpp): closes the previous pass's head
    // sample. The entity aim point is logged alongside as a cross-check.
    float aim_dy = -1000.0f;  // sentinel: unreadable this pass
    if (feet_valid &&
        diag::Accessible(entity + gamecam::ENTITY_AIM_OFFSET, 12, false))
        aim_dy = reinterpret_cast<const float*>(
                     entity + gamecam::ENTITY_AIM_OFFSET)[1] - feet[1];
    eyeheight::OnFrame(aim_dy);


    // Hand-band window: counts down only on passes where the body drew.
    if (vrmod::config.handband_request > 0) {
        handband_passes = vrmod::config.handband_request;
        vrmod::config.handband_request = 0;
        for (int i = 0; i < HANDBAND_MAX_DRAWS; i++)
            handband_stats[i] = HandBandStat{};
        handband_seen_passes = 0;
        handband_bone_passes = 0;
        handband_max_index = 0;
        handband_drew_last_pass = false;
        diag::Log("handband: armed for %d body-drawing passes (~2 per game tick while drawing; weapon must be equipped for bone distances)",
                  handband_passes);
    } else if (handband_passes > 0 && handband_drew_last_pass) {
        handband_passes--;
        if (handband_passes == 0)
            HandBandReport();
    }
    handband_drew_last_pass = false;

}


// Every WORLD SetTransform (world-space passes only - the call site skips
// RHW/passthrough). ret = the game's call site; boxed = the minimap radar's
// pass (its arrows come from the body-root site, see the arm hide).
inline void OnWorldTransform(const D3DMATRIX& m, void* ret, bool boxed) {
    current_world = m;
    current_world_valid = true;
    OnWorldTransformArmHide(m, ret, boxed);
    // Head height for psobbvr_eyeheight.hpp: 0x844AFD sets exactly two
    // character matrices, the root at the feet and the head cluster
    // (~+16), so a set from there near our feet and 3..30 units above them
    // is the head. Boxed (radar) passes are skipped: our arrow at height 0
    // would match whenever we stand below 0.
    if (!boxed && live_feet != nullptr &&
        reinterpret_cast<uintptr_t>(ret) == BODY_ROOT_W_SET_SITE) {
        const float hdx = m._41 - live_feet[0];
        const float hdy = m._42 - live_feet[1];
        const float hdz = m._43 - live_feet[2];
        if (hdy > 3.0f && hdy < 30.0f && hdx * hdx + hdz * hdz < 6.25f)
            eyeheight::OnSample(hdy);
    }
}



}  // namespace partmap
