#pragma once

// Closing the character's fist around the held weapon, with IK arms on.
//
// Once per pass, for each hand, the held weapon's snapshot and the hand's
// welded mesh (both in that hand's grip frame once the weapon is seated) go
// through the fist-closing maths; the result, one move per welded vertex of
// the hand, is handed to the hand-draw substitution, which writes it into a
// copy of each hand draw's vertices (the game's own buffers are never
// written). The result is recomputed only when something it depends on
// changes, and eased in over a few passes so the fingers never pop.
//
// Hands: side 0 = left, 1 = right (the hands module's numbering). The weapon
// snapshot numbers them the other way round (0 main hand = right), so the
// pairing is made here.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "psobbvr_eyeheight.hpp"
#include "psobbvr_handclose_adapt.hpp"
#include "psobbvr_hands.hpp"
#include "psobbvr_ikarms.hpp"
#include "psobbvr_log.hpp"
#include "psobbvr_vr.hpp"
#include "psobbvr_weapongrip.hpp"
#include "psobbvr_weaponsnap.hpp"

namespace closefist {

// Passes a new result takes to reach full strength.
constexpr int EASE_PASSES = 8;
// Passes a changed tuning value must hold still before the hand is
// recomputed, so dragging a value does not recompute on every step.
constexpr int DEBOUNCE_PASSES = 4;
constexpr int MAX_W = handweld::MAX_WELDED;

// What a result depends on that changes on a discrete event: recomputed at
// once. All members are 4 bytes, so the struct compares bytewise.
struct StableKey {
    uint32_t hand_generation;
    uint32_t snap_generation;
    uint32_t kind;
    uint32_t class_id;
    uint32_t welded_count;
    uint32_t has_axis;
};
// What it depends on that the player tunes live: recomputed once it has
// held still for a few passes.
struct TuneKey {
    float seat[16];  // the weapon's full matrix in the grip frame: scale x seat
    float weapon_scale;
    float world_scale;
    float options[5];  // gap, reach, cap, fade (cm) and the core's inner_cos
    float axis[6];     // the gun's channel line: point, direction
};

// Zero-initialized on purpose (no member initializers): lives in the DLL's
// uninitialized data.
struct HandState {
    bool have_applied;           // a result has been computed for the keys below
    bool active;                 // a reshape is set on the hand
    StableKey stable;            // of the last result
    TuneKey tune;                // of the last result
    TuneKey pending;             // a tuning change waiting out its debounce
    int pending_age;
    int ease;                    // passes since the result arrived, capped at EASE_PASSES
    int count;                   // welded vertices of the result
    uint32_t generation;         // the hand mesh generation it was made for
    handclose::Stats stats;
    double ms;
    float from[MAX_W][3], to[MAX_W][3], shown[MAX_W][3];
    float nrm_from[MAX_W][3], nrm_to[MAX_W][3], nrm_shown[MAX_W][3];
};
inline HandState hs[2];
inline uint32_t recomputes[2];
inline const char* SideName(int side) { return side == 0 ? "left" : "right"; }

inline bool KeysSame(const void* a, const void* b, size_t n) { return memcmp(a, b, n) == 0; }

// The hand's plain, unreshaped self.
inline void Clear(int side) {
    HandState& h = hs[side];
    if (h.active)
        hands::IkSetReshape(side, nullptr, nullptr, 0);
    h.active = false;
    h.have_applied = false;
    h.pending_age = 0;
    h.ease = 0;
}

inline void ClearAll() {
    for (int side = 0; side < 2; side++)
        Clear(side);
    hands::ik_reshape_enabled = false;
}

inline void Normalize3(float v[3]) {
    const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-12f) {
        v[0] /= l; v[1] /= l; v[2] /= l;
    }
}

// This pass's easing step: the shown moves and normals between the result's
// starting and final ones.
inline void Ease(HandState& h) {
    if (h.ease < EASE_PASSES)
        h.ease++;
    const float e = (float)h.ease / (float)EASE_PASSES;
    for (int i = 0; i < h.count; i++) {
        for (int c = 0; c < 3; c++) {
            h.shown[i][c] = h.from[i][c] + (h.to[i][c] - h.from[i][c]) * e;
            h.nrm_shown[i][c] = h.nrm_from[i][c] + (h.nrm_to[i][c] - h.nrm_from[i][c]) * e;
        }
        Normalize3(h.nrm_shown[i]);
    }
}

// The weapon's matrix into the grip frame (weapon scale x seat, row-vector)
// as doubles.
inline void WeaponMatrix(const D3DMATRIX& seat, float scale, double T[16]) {
    const float* m = &seat._11;
    for (int i = 0; i < 16; i++)
        T[i] = (i < 12) ? (double)m[i] * (double)scale : (double)m[i];
}

// One hand's pass: clears it, keeps it, or recomputes and eases it. main_seat:
// the main hand's seat (identity for guns); the left hand takes its mirror. A
// gun the hand is moved onto ([vr] gun_seat, weapongrip::GunSeatForHand) is
// held on that hand's gun seat instead, with its handle line as the axis.
inline void StepHand(int side, const D3DMATRIX& main_seat) {
    HandState& h = hs[side];
    const int snap_side = side == 1 ? 0 : 1;
    const std::shared_ptr<const weaponsnap::HeldGeometry> held = weaponsnap::Held(snap_side);
    const hands::IkWelded hw = hands::Welded(side);
    const bool usable = held != nullptr && !held->draws.empty() && hw.valid && hw.mesh != nullptr &&
                        hw.mesh->vertex_count > 0 && hw.mesh->vertex_count <= MAX_W &&
                        held->kind == weapongrip::current_kind;
    const bool gun = usable && weapongrip::KindIsGun(held->kind);
    weapongrip::HeldSeat gun_seat;
    if (gun && vrmod::config.gun_seat == 1)
        gun_seat = weapongrip::GunSeatForHand(side, ikarms::Active());
    const bool seated = gun_seat.Moves();
    // Without the gun seat a gun is held in the right hand only. Where the
    // game does not draw weapons the hand stays plain.
    if (!usable || weapongrip::weapons_hidden || (gun && side == 0 && !seated)) {
        Clear(side);
        return;
    }

    D3DMATRIX seat = main_seat;
    if (side == 0)
        seat = weapongrip::MirroredSeat(seat);
    if (seated)
        seat = gun_seat.seat;
    const float scale = vrmod::config.weapon_scale;
    const float world = vrmod::config.world_scale;

    // A gun's axis: the fist channel's line, or on the gun seat its handle line.
    double axis_o[3] = {0, 0, 0}, axis_a[3] = {0, 0, 1};
    if (gun && seated) {
        if (!gunseat::HandleLine(gun_seat.line_kind, &seat._11, scale, axis_o, axis_a)) {
            Clear(side);
            return;
        }
    } else if (gun) {
        float axis_point[2] = {0, 0}, axis_dir[3] = {0, 0, 1}, axis_anchor[3] = {0, 0, 1};
        if (!eyeheight::HandChannel(axis_point, axis_dir, axis_anchor)) {
            Clear(side);  // no fist channel: the hand stays plain
            return;
        }
        axis_o[0] = axis_point[0]; axis_o[1] = axis_point[1];
        for (int c = 0; c < 3; c++)
            axis_a[c] = axis_dir[c];
    }

    StableKey stable;
    memset(&stable, 0, sizeof(stable));
    stable.hand_generation = hw.generation;
    stable.snap_generation = held->generation;
    stable.kind = held->kind;
    stable.class_id = (uint32_t)hw.class_id;
    stable.welded_count = (uint32_t)hw.mesh->vertex_count;
    stable.has_axis = gun ? 1u : 0u;
    TuneKey tune;
    memset(&tune, 0, sizeof(tune));
    memcpy(tune.seat, &seat, sizeof(tune.seat));
    tune.weapon_scale = scale;
    tune.world_scale = world;
    tune.options[0] = vrmod::config.hand_close_gap_cm;
    tune.options[1] = vrmod::config.hand_close_reach_cm;
    tune.options[2] = vrmod::config.hand_close_cap_cm;
    tune.options[3] = vrmod::config.hand_close_fade_cm;
    tune.options[4] = (float)handclose::Options().inner_cos;
    if (gun) {
        for (int c = 0; c < 3; c++) {
            tune.axis[c] = (float)axis_o[c];
            tune.axis[3 + c] = (float)axis_a[c];
        }
    }

    bool recompute = !h.have_applied || !KeysSame(&stable, &h.stable, sizeof(stable));
    if (!recompute) {
        if (KeysSame(&tune, &h.tune, sizeof(tune))) {
            h.pending_age = 0;
        } else {
            if (h.pending_age == 0 || !KeysSame(&tune, &h.pending, sizeof(tune))) {
                h.pending = tune;
                h.pending_age = 1;
            } else {
                h.pending_age++;
            }
            if (h.pending_age > DEBOUNCE_PASSES)
                recompute = true;
        }
    }

    if (recompute) {
        const auto t0 = std::chrono::steady_clock::now();
        handclose::Options options;
        options.gap_cm = tune.options[0];
        options.reach_cm = tune.options[1];
        options.cap_cm = tune.options[2];
        options.fade_cm = tune.options[3];
        double T[16];
        WeaponMatrix(seat, scale, T);
        handclose::Axis axis;
        if (gun) {
            for (int c = 0; c < 3; c++) {
                axis.o[c] = axis_o[c];
                axis.a[c] = axis_a[c];
            }
        }
        const handclose::adapt::Closed closed = handclose::adapt::Close(
            *hw.mesh, handclose::adapt::WeaponParts(*held), T, gun ? &axis : nullptr,
            0.01 * (double)world, options);
        const int n = hw.mesh->vertex_count;
        // Start from what is shown now when the same mesh is already reshaped,
        // else from the plain hand.
        const bool continuing = h.active && h.generation == hw.generation && h.count == n;
        for (int i = 0; i < n; i++) {
            for (int c = 0; c < 3; c++) {
                h.from[i][c] = continuing ? h.shown[i][c] : 0.0f;
                h.nrm_from[i][c] = continuing ? h.nrm_shown[i][c] : closed.normals_rest[(size_t)i * 3 + c];
                h.to[i][c] = closed.moves[(size_t)i * 3 + c];
                h.nrm_to[i][c] = closed.normals[(size_t)i * 3 + c];
            }
        }
        h.count = n;
        h.generation = hw.generation;
        h.ease = 0;
        h.stable = stable;
        h.tune = tune;
        h.pending_age = 0;
        h.have_applied = true;
        h.stats = closed.fist.stats;
        h.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        recomputes[side]++;
        diag::Log("closefist: %s hand, kind 0x%02X, class %d: inner %d engaged %d inside %d unresolved %d "
                  "refined %d capped %d wrist %d, largest move %.2f cm, %.2f ms (computation %u)",
                  SideName(side), (unsigned)held->kind, hw.class_id, h.stats.inner, h.stats.engaged,
                  h.stats.inside, h.stats.unresolved, h.stats.refined, h.stats.capped, h.stats.wrist,
                  h.stats.max_move_cm, h.ms, (unsigned)recomputes[side]);
    }

    if (h.have_applied) {
        Ease(h);
        hands::IkSetReshape(side, h.shown, h.nrm_shown, h.generation);
        h.active = true;
    }
}

// Once per pass, after the weapon module's and the hands module's per-pass
// state and the IK arms' targets are current: the weapon snapshot and the
// hand meshes it reads are the last pass's draw captures, so a result is at
// most one pass behind what is on screen.
inline void Step() {
    if (vrmod::config.hand_close == 0 || !ikarms::Active()) {
        if (hands::ik_reshape_enabled || hs[0].active || hs[1].active)
            ClearAll();
        return;
    }
    hands::ik_reshape_enabled = true;
    const D3DMATRIX seat = weapongrip::MeleeSlide();
    for (int side = 0; side < 2; side++)
        StepHand(side, seat);
}

// One line per hand: the keys, the last result, the ease and the
// generations.
inline void LogStatus() {
    diag::Log("handclosestat: hand_close %d, ik active %d, substitution %s; gap %.2f reach %.2f cap %.2f "
              "fade %.2f cm; snapshot generation %u, weapon kind 0x%02X",
              vrmod::config.hand_close, ikarms::Active() ? 1 : 0, hands::ik_reshape_enabled ? "on" : "off",
              vrmod::config.hand_close_gap_cm, vrmod::config.hand_close_reach_cm,
              vrmod::config.hand_close_cap_cm, vrmod::config.hand_close_fade_cm,
              (unsigned)weaponsnap::Generation(), (unsigned)weapongrip::current_kind);
    for (int side = 0; side < 2; side++) {
        const HandState& h = hs[side];
        const hands::IkWelded hw = hands::Welded(side);
        const auto held = weaponsnap::Held(side == 1 ? 0 : 1);
        diag::Log("handclosestat: %s hand: mesh %s generation %u class %d (%d welded); snapshot %s generation %u "
                  "kind 0x%02X; reshape %s, ease %d/%d, %u computations",
                  SideName(side), hw.valid ? "valid" : "none", (unsigned)hw.generation, hw.class_id,
                  hw.mesh != nullptr ? hw.mesh->vertex_count : 0, held ? "held" : "none",
                  held ? (unsigned)held->generation : 0u, held ? (unsigned)held->kind : 0u,
                  h.active ? "set" : "plain", h.ease, EASE_PASSES, (unsigned)recomputes[side]);
        if (!h.have_applied)
            continue;
        diag::Log("handclosestat: %s hand: key: hand generation %u, snapshot generation %u, kind 0x%02X, class "
                  "%u, %u welded, axis %u; weapon scale %.3f, world scale %.3f, options %.2f/%.2f/%.2f/%.2f; "
                  "seat row 3 %.4f %.4f %.4f; waiting change age %d",
                  SideName(side), (unsigned)h.stable.hand_generation, (unsigned)h.stable.snap_generation,
                  (unsigned)h.stable.kind, (unsigned)h.stable.class_id, (unsigned)h.stable.welded_count,
                  (unsigned)h.stable.has_axis, h.tune.weapon_scale, h.tune.world_scale, h.tune.options[0],
                  h.tune.options[1], h.tune.options[2], h.tune.options[3], h.tune.seat[12],
                  h.tune.seat[13], h.tune.seat[14], h.pending_age);
        diag::Log("handclosestat: %s hand: last result: inner %d engaged %d inside %d unresolved %d refined %d "
                  "capped %d wrist %d, largest move %.2f cm, %.2f ms",
                  SideName(side), h.stats.inner, h.stats.engaged, h.stats.inside, h.stats.unresolved,
                  h.stats.refined, h.stats.capped, h.stats.wrist, h.stats.max_move_cm, h.ms);
    }
}


}  // namespace closefist
