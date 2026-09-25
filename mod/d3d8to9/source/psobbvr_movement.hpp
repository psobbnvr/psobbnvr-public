#pragma once

// VR locomotion shaping on the player's movement code, applied while the
// game-camera takeover is enabled and reverted otherwise.
//
//   [vr] stick_locomotion  - analog stick walking: direction and speed
//                            from the left stick, facing turned only by
//                            the right stick and head_move
//   [vr] turn_speed_scale  - slower keyboard rotation
//   [vr] back_strafe       - back input walks backwards (no 180 turn)
//   [vr] side_turn         - turn-key-only input turns in place instead
//                            of running 90 deg to the side
//
// Player movement internals (player vtable 0x00B39460; locomotion lives in
// 0x685000-0x6B0000):
//
// - 0x0070A60C fills the keyboard "virtual stick", cdecl(short* side,
//   short* fwd), writing DAT_00A94330 (side) / DAT_00A9432C (fwd). It
//   re-seeds its ramp from those globals, so they must never be rewritten
//   (the character would never stop walking).
// - Standing steer 0x0069B728: target = bams(atan2(side, fwd)) + camera
//   yaw ([0x00A48A54]+0x94, the camera's orbit angle, ~opposite the
//   facing). Side input rotates in place via step(facing, target, rate) at
//   0x0069B89B (rate = imm32 0x800 at 0x0069B890) and starts moving once
//   aligned; fwd/back input starts moving at once with moveDir (+0x3FC) =
//   target.
// - Moving: velocity +0x30C/+0x314 = speed * (sin, cos)(moveDir) moves the
//   character, built by 0x0069BABC (run) / 0x006A14E0 (walk). The ticks
//   0x0069BA18/0x006A1464 plant dest (+0x404/+0x40C) = feet +
//   speed*dir(moveDir) via 0x0069BB6C and send it to 0x008003E0 (msg 0x40,
//   "walk to"). The facing ticks 0x006A1DE8/0x006A1790 recompute moveDir =
//   angle(feet->dest) (0x007A90A8), set "arrived" (+0x6C4 bit 2, needed to
//   stop) when dist < 2*speed, and chase the facing toward moveDir via
//   0x007A8CD0 (half the gap per tick, a quarter inside 0x3000).
//
// Raw input is never rewritten, so moveDir, velocity, dest and the stop
// path stay consistent. Instead the facing-steer and midpoint call sites
// are redirected to stubs that substitute the facing target, and the
// camera-yaw reads are patched to read g_steer_yaw (the walk-direction
// reference). The input-fill detour decides both each tick. Velocity
// write sites are hooked to scale speed (ScaledSpeedGuard). Only our own
// entity is steered (TickEntityScope). The gamepad branch reads its own
// direction (0x006A7CAC) and is unaffected.
//
// All code writes happen on the game's thread, so patched bytes never run concurrently.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <windows.h>

#include "MinHook.h"
#include "psobbvr_gamecam.hpp"
#include "psobbvr_probe.hpp"
#include "psobbvr_vr.hpp"

namespace movement {

// ---- addresses (see the header comment) ---------------------------------

// imm32 of "PUSH 0x800" in the standing steer: the in-place turn rate in
// binary angles per tick (0x800 = 11.25 deg = 337 deg/s at 30 FPS).
constexpr uintptr_t STAND_TURN_RATE_IMM_ADDR = 0x0069B890;
constexpr uint32_t STAND_TURN_RATE_STOCK = 0x800;

// The functions our stubs wrap (originals called by absolute address).
constexpr uintptr_t INPUT_FILL_ADDR = 0x0070A60C;  // keyboard virtual stick
constexpr uintptr_t ANGLE_STEP_ADDR = 0x007A8C74;  // step(cur, tgt, rate)
constexpr uintptr_t MOVING_TURN_ADDR = 0x007A8CD0; // facing chase while moving
constexpr uintptr_t MIDPOINT_ADDR = 0x007A8CB8;    // cur + wrap(tgt-cur)/2

// CALL rel32 sites redirected to the stubs (rel32 at site+1).
constexpr uintptr_t SITE_STAND_STEP = 0x0069B89B;  // in standing steer 0x0069B728
constexpr uintptr_t SITE_TURN_RUN = 0x006A1E07;    // in facing tick 0x006A1DE8
constexpr uintptr_t SITE_TURN_WALK = 0x006A17AF;   // in facing tick 0x006A1790
// The facing ticks rebuild the velocity from the midpoint of (facing,
// moveDir), which is ~90 deg sideways while reversing; bypassed to moveDir
// there.
constexpr uintptr_t SITE_MID_RUN = 0x006A1E16;     // in facing tick 0x006A1DE8
constexpr uintptr_t SITE_MID_WALK = 0x006A17BE;    // in facing tick 0x006A1790

// Where the game reads the camera yaw it adds to the input angle: 6-byte
// reg,[reg+0x94] forms, patched to the same-length disp32 form reading
// g_steer_yaw. The keyboard is merged into pad record 0 (DAT_00AAE75C,
// pointed to by player+0x634) and the moving steers take the pad branch,
// so the pad poller's read (0x007BECD4, yaw loaded at 0x7BF915, added at
// 0x7BF947) governs forward-walk steering. These are all the camera-yaw
// reads in the player module, plus the pad poller:
constexpr uintptr_t SITE_CAMYAW_STAND = 0x0069B87C;  // ADD EBP,[EDX+0x94] in 0x0069B728 (stand steer)
constexpr uintptr_t SITE_CAMYAW_WALK = 0x0069BC4D;   // ADD EBX,[EDX+0x94] in 0x0069BBD0 (walk steer, router 0x69B93C)
constexpr uintptr_t SITE_CAMYAW_RUN = 0x006A161D;    // ADD EBX,[EDX+0x94] in 0x006A15A0 (run steer, router 0x6A139C)
constexpr uintptr_t SITE_CAMYAW_ATTACK = 0x0069C372; // ADD EDX,[EBP+0x94] in 0x0069C0C8 (attack-move steer)
constexpr uintptr_t SITE_CAMYAW_PAD = 0x007BF915;    // MOV EDX,[EDX+0x94] in 0x007BECD4 (pad poller)

// Player base movement speed, float at entity+0x6B8 (the run velocity
// getter 0x0069788C scales it by status via 0x006978A0; the facing ticks
// read it raw). Written only at spawn/equip, so scaling saves and restores
// rather than scaling in place (which would compound).
constexpr uintptr_t SPEED_FIELD_OFFSET = 0x6B8;

// Camera yaw offset in the camera object ([0x00A48A54]).
constexpr uintptr_t CAMERA_YAW_OFFSET = 0x94;

// Reverse cone: enter beyond 112.5 deg off the facing (back key and both
// back-diagonals), leave below 101.25 deg.
constexpr int REVERSE_ENTER_BAMS = 0x5000;
constexpr int REVERSE_STAY_BAMS = 0x4800;

// Side-turn carrot: with turn-key-only input the game's target is 90 deg
// off the current facing, which bounces under the rate-capped chase. The
// target is pinned 67.5 deg out instead: past 0x3000 the moving chase
// takes its half-gap branch (full capped rate), the gap never reaches the
// hold-behind cone, and the standing steer never aligns and starts a walk.
constexpr int SIDE_TURN_CARROT_BAMS = 0x3000;

// The stand steer's walk-start test: each axis / 128.0 [0x93DA88] is
// compared with 0.1 [0x93DA84], so fwd/back starts a walk at |fwd| >= 13.
// Side input (tested first) steps the facing and starts the walk only when
// it lands on the target (cmp eax,ebp / je at 0x0069B8A6/A8), which never
// happens while the strafe regime holds the facing. So during strafe
// movement the je (0x74) becomes jmp (0xEB): the aligned start runs with
// the exact requested moveDir. Restored otherwise (the keyboard spot turn
// needs rotate-without-walking). Flipped from the input fill, earlier in
// the same tick than the steer runs.
constexpr uintptr_t STAND_ALIGNED_JE_ADDR = 0x0069B8A8;
constexpr uint8_t STAND_ALIGNED_JE_STOCK = 0x74;
constexpr uint8_t STAND_ALIGNED_JMP = 0xEB;
inline bool stand_start_forced = false;
inline void SetStandStartForced(bool want);  // defined below WriteBytes

// ---- per-tick state (written by the input detour, read by the stubs
// later in the same tick) ---------------------------------------------------
inline bool reverse_active = false;
inline int face_target_bams = 0;  // A: the request with the 180 removed
inline bool side_turn_active = false;  // turn-key-only input this frame
inline int side_turn_target_bams = 0;  // live carrot: facing -/+ the carrot
                                       // distance, recomputed every fill

// The yaw the patched camera-yaw reads fetch, published on every input
// fill (which runs right before the steers).
inline int32_t g_steer_yaw = 0;

// Speed-field scaling state (MaintainSpeedScale).
inline bool speed_scaled = false;
inline float saved_base_speed = 0.0f;
inline float written_speed = 0.0f;

inline int Wrap16(int a) { return (int)(int16_t)(a & 0xFFFF); }

// Legacy stick mix (stick_locomotion off; set by controller::OnFrame): the
// sticks as virtual-stick axis values (64 = walk, 128 = run), used only
// when the keyboard gave nothing.
inline bool stick_move_active = false;
inline int stick_move_side = 0;
inline int stick_move_fwd = 0;
// Per-tick turn-rate override from right-stick deflection; < 0 = use
// turn_speed_scale. Read through Scale().
inline float stick_turn_scale = -1.0f;

// Analog stick locomotion (stick_locomotion on; set by controller::OnFrame):
// InputFillDetour injects the direction at full run magnitude, scales the
// speed by deflection, and aims the facing at facing + right-stick turn
// step, independent of the walk direction. Side carries
// controller_side_sign, fwd is negative forward, turn is signed like side.
inline bool stick_raw_active = false;  // any stick input past deadzone
inline float stick_raw_side = 0.0f;    // left stick side, -1..1
inline float stick_raw_fwd = 0.0f;     // left stick fwd, -1..1 (neg = fwd)
inline float stick_raw_turn = 0.0f;    // right stick turn, -1..1
// Strafe regime this tick: the stubs return the target / moveDir directly,
// since the per-tick target is already rate-limited (the stock chase would
// lag it).
inline bool strafe_mode = false;
inline int strafe_face_target = 0;  // facing + turn step (+ hold/follow steps)

inline float Scale() {
    const float s = stick_turn_scale >= 0.0f ? stick_turn_scale
                                             : vrmod::config.turn_speed_scale;
    return s < 0.05f ? 0.05f : (s > 1.0f ? 1.0f : s);
}

// Entity motion fields.
constexpr uintptr_t VEL_X_OFFSET = 0x30C;    // velocity x (float)
constexpr uintptr_t VEL_Z_OFFSET = 0x314;    // velocity z (float)
constexpr uintptr_t DEST_X_OFFSET = 0x404;   // walk-to destination x (float)
constexpr uintptr_t DEST_Z_OFFSET = 0x40C;   // walk-to destination z (float)
constexpr uintptr_t TRACE_ENTITY_SPAN = 0x6C8;  // one read-guard for all fields

constexpr bool TraceOn() { return false; }

// ---- strafe-mode analog speed --------------------------------------------
// All four velocity write sites are hooked and ScaledSpeedGuard scales the
// speed sources only for each original call. Scaling the field per tick
// loses races with the game's stat refreshes, and scaling the velocity
// afterwards is too late (these functions integrate position before
// returning). MaintainSpeedScale remains for the keyboard path and
// turn-only.
constexpr uintptr_t RUN_VEL_BUILD_ADDR = 0x0069BABC;   // run-mode velocity build
constexpr uintptr_t WALK_VEL_BUILD_ADDR = 0x006A14E0;  // walk-mode velocity build
// The facing ticks also rebuild the velocity each moving tick (raw speed
// field x dir(midpoint)). Their entry bytes don't overlap the call-site
// redirects inside them.
constexpr uintptr_t FACING_TICK_RUN_ADDR = 0x006A1DE8;
constexpr uintptr_t FACING_TICK_WALK_ADDR = 0x006A1790;

inline float strafe_vel_scale = -1.0f;  // <0 = inactive (stock speeds)
inline uintptr_t strafe_entity = 0;     // the entity the scale applies to
inline int strafe_winddown_ticks = 0;   // facing-hold ticks after release

// Whose movement routine is running. The stubs sit in routines every
// player-class entity runs (remote players, party NPCs) and receive only
// angles, so the detours on the standing steer and the two facing ticks
// record the ticking entity; stubs substitute only for ours. Unresolvable
// counts as ours, so a failed hook never disables our own steering.
inline uintptr_t tick_entity = 0;   // entity whose tick is running (0 = none)
inline bool tick_foreign = false;   // that entity is not ours
inline bool foreign_logged = false;
struct TickEntityScope {
    uintptr_t saved_entity;
    bool saved_foreign;
    explicit TickEntityScope(void* self) {
        saved_entity = tick_entity;
        saved_foreign = tick_foreign;
        tick_entity = reinterpret_cast<uintptr_t>(self);
        const uintptr_t ours = gamecam::ResolveEntity();
        tick_foreign = tick_entity != 0 && ours != 0 && tick_entity != ours;
        if (tick_foreign) {
            if (!foreign_logged) {
                foreign_logged = true;
                diag::Log("movement: movement tick for another player-class entity "
                          "(%08X, ours %08X) - stock steering for it (first sighting)",
                          (unsigned)tick_entity, (unsigned)ours);
            }
        }
    }
    ~TickEntityScope() {
        tick_entity = saved_entity;
        tick_foreign = saved_foreign;
    }
};

// The builders return a movement-permitted flag in EAX (0/1; 0x694198
// tests walkability of feet + velocity) that the callers branch on, so the
// detours must pass it through.
using VelBuildFn = uint32_t(__fastcall*)(void* self, void* edx);
inline VelBuildFn original_run_vel = nullptr;
inline VelBuildFn original_walk_vel = nullptr;
inline VelBuildFn original_run_facing_tick = nullptr;
inline VelBuildFn original_walk_facing_tick = nullptr;

// The walk-speed float the walk builder multiplies, and the walk-to dest
// plant speed. Scaling the plant speed too keeps destinations
// proportionally close, so a released walk ends in one short scaled
// segment.
constexpr uintptr_t WALK_SPEED_GLOBAL_ADDR = 0x00A94324;
constexpr uintptr_t DEST_PLANT_SPEED_GLOBAL_ADDR = 0x00A94320;

// Scales the entity speed field, the walk-speed global and the plant
// speed for the duration of one hooked call, restoring them on return.
struct ScaledSpeedGuard {
    float* field = nullptr;
    float* walk_global = nullptr;
    float* plant_global = nullptr;
    float saved_field = 0.0f;
    float saved_global = 0.0f;
    float saved_plant = 0.0f;
    explicit ScaledSpeedGuard(void* self) {
        if (strafe_vel_scale < 0.0f || (uintptr_t)self != strafe_entity)
            return;
        field = reinterpret_cast<float*>(
            reinterpret_cast<uint8_t*>(self) + SPEED_FIELD_OFFSET);
        saved_field = *field;
        *field = saved_field * strafe_vel_scale;
        walk_global = reinterpret_cast<float*>(WALK_SPEED_GLOBAL_ADDR);
        saved_global = *walk_global;
        *walk_global = saved_global * strafe_vel_scale;
        plant_global = reinterpret_cast<float*>(DEST_PLANT_SPEED_GLOBAL_ADDR);
        saved_plant = *plant_global;
        *plant_global = saved_plant * strafe_vel_scale;
    }
    ~ScaledSpeedGuard() {
        if (field != nullptr)
            *field = saved_field;
        if (walk_global != nullptr)
            *walk_global = saved_global;
        if (plant_global != nullptr)
            *plant_global = saved_plant;
    }
};

inline uint32_t __fastcall RunVelBuildDetour(void* self, void* edx) {
    ScaledSpeedGuard guard(self);
    return original_run_vel(self, edx);
}

inline uint32_t __fastcall WalkVelBuildDetour(void* self, void* edx) {
    ScaledSpeedGuard guard(self);
    return original_walk_vel(self, edx);
}

inline uint32_t __fastcall RunFacingTickDetour(void* self, void* edx) {
    TickEntityScope scope(self);
    ScaledSpeedGuard guard(self);
    return original_run_facing_tick(self, edx);
}

inline uint32_t __fastcall WalkFacingTickDetour(void* self, void* edx) {
    TickEntityScope scope(self);
    ScaledSpeedGuard guard(self);
    return original_walk_facing_tick(self, edx);
}

inline void TraceStub(uintptr_t, int, int, int, bool) {}
inline void TraceForeign(uintptr_t, int, int, int) {}

// ---- stubs ---------------------------------------------------------------

// Return types are int (raw EAX passthrough): the standing steer stores
// the step result to facing as a full dword and compares full registers.
using AngleStepFn = int(__cdecl*)(int cur, int tgt, int rate);
using MovingTurnFn = uint32_t(__cdecl*)(uint32_t cur, uint32_t tgt);
using MidpointFn = uint32_t(__cdecl*)(uint32_t cur, uint32_t tgt);
using InputFillFn = void(__cdecl*)(int16_t* side, int16_t* fwd);

inline InputFillFn original_input_fill = nullptr;

// Near-180 rule: with the target more than 135 deg behind the facing, hold
// instead of chasing. Stock math is bistable near 180 (the midpoint flips
// +/-90 deg on noise and the facing bounces). With back_strafe this only
// happens while reversing or just after; stateless so it also covers
// ticks after reverse_active has cleared.
inline bool HoldBehind(int cur, int tgt) {
    if (!vrmod::config.back_strafe)
        return false;
    const int d = Wrap16(tgt - cur);
    return d > 0x6000 || d < -0x6000;
}

// head_move 2: when a stub hands the game the facing that carries this
// tick's head-follow step, move the step into gamecam::follow_offset (and
// hold_prev_facing), so the view target is unchanged whatever order the
// tick and gamecam::Apply run in. Consumed once.
inline void AbsorbFollowStep(int out) {
    if (gamecam::follow_pending_step == 0 ||
        (out & 0xFFFF) != (gamecam::follow_pending_target & 0xFFFF))
        return;
    gamecam::follow_offset =
        Wrap16(gamecam::follow_offset + gamecam::follow_pending_step);
    if (gamecam::hold_prev_valid)
        gamecam::hold_prev_facing =
            Wrap16(gamecam::hold_prev_facing + gamecam::follow_pending_step);
    gamecam::follow_pending_step = 0;
}

// Standing in-place step: aim at A while reversing (pure back has no side
// input and never gets here; diagonals rotate toward their side).
inline int __cdecl StandStepStub(int cur, int tgt, int rate) {
    if (tick_foreign) {
        const int out = reinterpret_cast<AngleStepFn>(ANGLE_STEP_ADDR)(cur, tgt, rate);
        if (TraceOn())
            TraceForeign((uintptr_t)_ReturnAddress(), cur, tgt, out);
        return out;
    }
    // Strafe regime: land exactly on the per-tick target. The caller's
    // aligned check compares against the request, which this never
    // matches, so turn-only input rotates without starting a walk.
    if (strafe_mode) {
        const int out = strafe_face_target & 0xFFFF;
        AbsorbFollowStep(out);
        if (TraceOn())
            TraceStub((uintptr_t)_ReturnAddress(), cur, out, out, false);
        return out;
    }
    if (reverse_active)
        tgt = face_target_bams & 0xFFFF;
    if (HoldBehind(cur, tgt)) {
        if (TraceOn())
            TraceStub((uintptr_t)_ReturnAddress(), cur, tgt, cur, true);
        return cur & 0xFFFF;
    }
    const int out = reinterpret_cast<AngleStepFn>(ANGLE_STEP_ADDR)(cur, tgt, rate);
    if (TraceOn())
        TraceStub((uintptr_t)_ReturnAddress(), cur, tgt, out, false);
    return out;
}

// While-moving facing chase: aim at A while reversing; below scale 1 the
// stock half/quarter-gap step is scaled and capped at the scaled stand
// rate.
inline uint32_t __cdecl MovingTurnStub(uint32_t cur, uint32_t tgt) {
    if (tick_foreign) {
        const uint32_t out = reinterpret_cast<MovingTurnFn>(MOVING_TURN_ADDR)(cur, tgt);
        if (TraceOn())
            TraceForeign((uintptr_t)_ReturnAddress(), (int)cur, (int)tgt, (int)out);
        return out;
    }
    if (strafe_mode) {
        const uint32_t out = (uint32_t)(strafe_face_target & 0xFFFF);
        AbsorbFollowStep((int)out);
        if (TraceOn())
            TraceStub((uintptr_t)_ReturnAddress(), (int)cur, (int)out, (int)out, false);
        return out;
    }
    if (reverse_active)
        tgt = (uint32_t)(face_target_bams & 0xFFFF);
    // Spot turn: the moving chase follows moveDir (toward dest), and with
    // the speed collapsed arrived ticks stop re-reading the request (dest
    // is planted from the global speed 0x00A94320), so use the live
    // carrot directly.
    else if (side_turn_active)
        tgt = (uint32_t)(side_turn_target_bams & 0xFFFF);
    if (HoldBehind((int)cur, (int)tgt)) {
        if (TraceOn())
            TraceStub((uintptr_t)_ReturnAddress(), (int)cur, (int)tgt, (int)cur, true);
        return cur & 0xFFFF;
    }
    uint32_t out;
    const float s = Scale();
    if (s >= 0.999f) {
        out = reinterpret_cast<MovingTurnFn>(MOVING_TURN_ADDR)(cur, tgt);
    } else {
        const int d = Wrap16((int)tgt - (int)cur);
        int step = (d > -0x3000 && d < 0x3000) ? d / 4 : d / 2;
        step = (int)(step * s);
        int cap = (int)(STAND_TURN_RATE_STOCK * s);
        if (cap < 1)
            cap = 1;
        if (step > cap)
            step = cap;
        else if (step < -cap)
            step = -cap;
        if (step == 0 && d != 0)
            step = d > 0 ? 1 : -1;
        out = (uint32_t)((int)cur + step) & 0xFFFF;
    }
    if (TraceOn())
        TraceStub((uintptr_t)_ReturnAddress(), (int)cur, (int)tgt, (int)out, false);
    return out;
}

// Velocity-direction midpoint in the facing ticks: when moveDir is behind
// the facing, the velocity follows moveDir instead of the blend (which is
// ~90 deg sideways there).
inline uint32_t __cdecl MidpointStub(uint32_t cur, uint32_t tgt) {
    if (tick_foreign) {
        const uint32_t out = reinterpret_cast<MidpointFn>(MIDPOINT_ADDR)(cur, tgt);
        if (TraceOn())
            TraceForeign((uintptr_t)_ReturnAddress(), (int)cur, (int)tgt, (int)out);
        return out;
    }
    // Strafe regime: always follow moveDir (the blend would pull toward
    // the held facing at any angle).
    if (strafe_mode) {
        if (TraceOn())
            TraceStub((uintptr_t)_ReturnAddress(), (int)cur, (int)tgt, (int)tgt, false);
        return tgt & 0xFFFF;
    }
    if (HoldBehind((int)cur, (int)tgt)) {
        if (TraceOn())
            TraceStub((uintptr_t)_ReturnAddress(), (int)cur, (int)tgt, (int)tgt, true);
        return tgt & 0xFFFF;
    }
    const uint32_t out = reinterpret_cast<MidpointFn>(MIDPOINT_ADDR)(cur, tgt);
    if (TraceOn())
        TraceStub((uintptr_t)_ReturnAddress(), (int)cur, (int)tgt, (int)out, false);
    return out;
}

inline bool patches_applied = false;  // set by OnFrame

// Holds the base-speed field at saved * scale while scale < 1, restoring
// at 1. A stat rewrite by the game is adopted, and the restore never
// overwrites a newer value. Used for back_speed_scale and
// side_turn_speed_scale.
inline void MaintainSpeedScale(uintptr_t entity, float scale) {
    float* spd = nullptr;
    if (entity != 0 && diag::Accessible(entity + SPEED_FIELD_OFFSET, 4, true))
        spd = reinterpret_cast<float*>(entity + SPEED_FIELD_OFFSET);
    if (scale < 0.999f && spd != nullptr) {
        if (speed_scaled && *spd != written_speed)
            speed_scaled = false;  // the game rewrote the stat - adopt it
        if (!speed_scaled)
            saved_base_speed = *spd;
        const float target = saved_base_speed * scale;
        if (*spd != target)
            *spd = target;
        written_speed = target;
        speed_scaled = true;
    } else if (speed_scaled) {
        if (spd != nullptr && *spd == written_speed)
            *spd = saved_base_speed;  // untouched since our write - undo it
        speed_scaled = false;
    }
}

inline void TraceInput(uintptr_t, int, int, int, int, int, const char*) {}

// Right-stick turn step for this tick (BAMS): squared curve up to
// stick_turn_deg_s; side > 0 rotates toward negative BAMS.
inline int StickTurnStepBams() {
    const float t = stick_raw_turn;
    if (t == 0.0f)
        return 0;
    const float step_max =
        vrmod::config.stick_turn_deg_s * (65536.0f / 360.0f) / 30.0f;
    return (int)lroundf(-t * fabsf(t) * step_max);
}

// Input detour. Never writes the stick globals (see the header). Per call:
// injects stick input, picks the regime (analog strafe, wind-down, or the
// keyboard path), sets the stubs' targets, publishes g_steer_yaw and
// maintains the speed scale.
inline void __cdecl InputFillDetour(int16_t* side, int16_t* fwd) {
    original_input_fill(side, fwd);

    // Legacy stick mix, injected only when the keyboard gave nothing, so
    // it is processed exactly like keys.
    const bool kb_had = *side != 0 || *fwd != 0;
    const char* trace_src = kb_had ? "kb" : "-";
    if (stick_move_active && !kb_had) {
        *side = (int16_t)stick_move_side;
        *fwd = (int16_t)stick_move_fwd;
        trace_src = "stick";
    }

    uintptr_t cam = 0;
    if (diag::Accessible(gamecam::CAMERA_PTR_ADDR, 4, false))
        cam = *reinterpret_cast<const uintptr_t*>(gamecam::CAMERA_PTR_ADDR);
    if (cam != 0 && !diag::Accessible(cam + CAMERA_YAW_OFFSET, 4, false))
        cam = 0;
    const uintptr_t entity = gamecam::ResolveEntity();
    const int raw_yaw =
        cam != 0 ? *reinterpret_cast<const int32_t*>(cam + CAMERA_YAW_OFFSET) : 0;
    const int facing =
        entity != 0
            ? *reinterpret_cast<const int32_t*>(entity + gamecam::ENTITY_FACING_OFFSET)
            : 0;
    const bool resolved = patches_applied && cam != 0 && entity != 0;

    // Analog strafe injection: the direction always at full run magnitude
    // (no sub-threshold stutter); speed is scaled separately. Keyboard
    // wins.
    bool v2_injected = false;
    bool v2_turn_only = false;
    if (vrmod::config.stick_locomotion && !kb_had && stick_raw_active) {
        const float sx = stick_raw_side, sy = stick_raw_fwd;
        const float mag = sqrtf(sx * sx + sy * sy);
        if (mag > 0.001f) {
            // Side-dominant starts go through the forced aligned start
            // (SetStandStartForced, STAND_ALIGNED_JE_ADDR).
            const float inv = 128.0f / mag;
            *side = (int16_t)lroundf(sx * inv);
            *fwd = (int16_t)lroundf(sy * inv);
            v2_injected = true;
            trace_src = "strafe";
        } else if (stick_raw_turn != 0.0f) {
            // Turn-only: a side input keeps the router alive and starts
            // nothing (StandStepStub); fwd stays exactly 0. Not while a
            // released walk is still stopping: that would drop the speed
            // scale and the tail would run at full speed. The wind-down
            // below stops it first (with the turn still applied).
            bool stopping = false;
            if (resolved && strafe_winddown_ticks > 0 &&
                diag::Accessible(entity, TRACE_ENTITY_SPAN, false)) {
                const float wx =
                    *reinterpret_cast<const float*>(entity + VEL_X_OFFSET);
                const float wz =
                    *reinterpret_cast<const float*>(entity + VEL_Z_OFFSET);
                stopping = wx * wx + wz * wz > 0.0025f;
            }
            if (!stopping) {
                *side = (int16_t)(stick_raw_turn > 0.0f ? 128 : -128);
                *fwd = 0;
                v2_injected = true;
                v2_turn_only = true;
                trace_src = "turn";
            }
        }
    }

    strafe_mode = v2_injected && resolved;
    if (strafe_mode && !v2_turn_only)
        strafe_winddown_ticks = 60;  // safety cap; the wind-down normally
        // ends when the velocity dies (a shorter cap than the stop would
        // let its tail run unscaled)

    // Wind-down after the stick releases: the walk is still stopping and
    // the stock chase would turn toward the stale moveDir, so the stubs
    // hold the current facing (plus the right-stick turn) and the speed
    // scale stays live until the motion dies. Keyboard input ends it.
    bool winddown = false;
    if (!strafe_mode && resolved && !kb_had && strafe_winddown_ticks > 0) {
        const bool first_wind_tick = strafe_winddown_ticks == 60;
        strafe_winddown_ticks--;
        float wx = 0.0f, wz = 0.0f;
        const bool have_e = diag::Accessible(entity, TRACE_ENTITY_SPAN, false);
        if (have_e) {
            wx = *reinterpret_cast<const float*>(entity + VEL_X_OFFSET);
            wz = *reinterpret_cast<const float*>(entity + VEL_Z_OFFSET);
        }
        const float vsq = wx * wx + wz * wz;
        if (vsq > 0.0025f) {
            winddown = true;
            strafe_mode = true;
            // Release drift clamp: on the release tick, move the dest to
            // a stop point 5 x scale^2 units along the velocity, so a light
            // tap stops at once (via the game's arrive -> stop path) and a
            // fast release keeps a little momentum.
            if (first_wind_tick && have_e && strafe_vel_scale >= 0.0f) {
                const float vmag = sqrtf(vsq);
                const float drift =
                    5.0f * strafe_vel_scale * strafe_vel_scale;  // game units
                const float fx =
                    *reinterpret_cast<const float*>(entity + gamecam::ENTITY_POS_OFFSET);
                const float fz = *reinterpret_cast<const float*>(
                    entity + gamecam::ENTITY_POS_OFFSET + 8);
                *reinterpret_cast<float*>(entity + DEST_X_OFFSET) =
                    fx + wx / vmag * drift;
                *reinterpret_cast<float*>(entity + DEST_Z_OFFSET) =
                    fz + wz / vmag * drift;
            }
        } else {
            strafe_winddown_ticks = 0;
        }
    }

    // Walk-start branch: forced only during strafe movement input.
    SetStandStartForced(strafe_mode && !winddown && !v2_turn_only);
    const int s = *side, f = *fwd;
    int steer = raw_yaw;
    float scale = 1.0f;
    gamecam::follow_pending_step = 0;  // re-published below on head-follow ticks

    // Attack view hold drain (gamecam::hold_offset): outside an attack,
    // turn the character back toward the view by a capped half-chase per
    // tick (through the facing target, or a direct facing write when
    // idle), published so the hold absorbs exactly that step.
    // Head-directed walking ([vr] head_move, stick locomotion only):
    // 1 = walk where the head points (the camera yaw), facing kept;
    // 2 = the same, and walking turns the character toward the head, kept
    // out of the view via gamecam::follow_offset. Mode 2 has no drain.
    const int head_move = vrmod::config.stick_locomotion ? vrmod::config.head_move : 0;
    const bool body_follows_head = head_move == 2;
    bool action_mode_ok = false, action_attacking = false;
    if (resolved &&
        diag::Accessible(entity + gamecam::ENTITY_ACTION_MODE_OFFSET, 2, false)) {
        const short mode = *reinterpret_cast<const short*>(
            entity + gamecam::ENTITY_ACTION_MODE_OFFSET);
        action_mode_ok = true;
        action_attacking = mode >= 5 && mode <= 8;  // 8 = a technique cast
    }
    int hold_step = 0;
    bool hold_attacking = false;
    if (resolved && vrmod::config.attack_view_hold && gamecam::hold_offset != 0 &&
        action_mode_ok) {
        hold_attacking = action_attacking;
        if (!hold_attacking && !body_follows_head) {
            hold_step = -gamecam::hold_offset / 2;
            if (hold_step == 0)
                hold_step = gamecam::hold_offset > 0 ? -1 : 1;
            if (hold_step > 0x800) hold_step = 0x800;    // 11 deg/tick
            if (hold_step < -0x800) hold_step = -0x800;
        }
    }
    gamecam::hold_drain_pending = hold_step > 0 ? hold_step : -hold_step;
    // A stick turn toward the side the character already faces (same sign
    // as the offset) shrinks the offset instead, so the view comes round
    // and the character gets only the excess, with no re-align this tick.
    // Other turns apply in full.
    auto hold_turn = [&](int turn_step) -> int {
        if (turn_step == 0 || hold_attacking || body_follows_head ||
            gamecam::hold_offset == 0 ||
            (turn_step > 0) != (gamecam::hold_offset > 0))
            return turn_step;
        const int tmag = turn_step > 0 ? turn_step : -turn_step;
        const int omag = gamecam::hold_offset > 0 ? gamecam::hold_offset
                                                  : -gamecam::hold_offset;
        const int c = tmag < omag ? tmag : omag;
        gamecam::hold_offset -= turn_step > 0 ? c : -c;
        hold_step = 0;
        gamecam::hold_drain_pending = 0;
        return turn_step - (turn_step > 0 ? c : -c);
    };
    if (strafe_mode && winddown) {
        reverse_active = false;
        side_turn_active = false;
        stick_turn_scale = 1.0f;
        // Hold the current facing plus the right-stick turn (a switch from
        // walking to turn-only passes through these ticks).
        const int wind_turn = hold_turn(StickTurnStepBams());
        strafe_face_target = Wrap16(facing + wind_turn + hold_step);
        face_target_bams = strafe_face_target;
        steer = head_move != 0 ? raw_yaw
                               : facing + 0x8000 - (hold_attacking ? 0 : gamecam::hold_offset);
        trace_src = "wind";
        // strafe_vel_scale carries over from the last movement tick.
    } else if (strafe_mode) {
        // Analog strafe regime: no reverse cone, spot turn or hold-behind
        // (those are keyboard-only).
        reverse_active = false;
        side_turn_active = false;
        // Full stand rate: the facing target is already rate-limited.
        stick_turn_scale = 1.0f;

        // Backwardness of the movement direction: 0 up to 90 deg off
        // forward, 1 at pure back. Blends the back slowdown.
        float back_b = 0.0f;
        if (!v2_turn_only) {
            const int input_bams =
                (int)lroundf(atan2f((float)s, (float)f) * (32768.0f / 3.14159265f));
            const int delta = Wrap16(input_bams + 0x8000);
            const int abs_delta = delta < 0 ? -delta : delta;
            if (abs_delta > 0x4000)
                back_b = (float)(abs_delta - 0x4000) / 16384.0f;
        }

        // Speed: deflection through a floor + square curve, eased toward
        // back_speed_scale going backwards, applied by ScaledSpeedGuard.
        // Turn-only collapses the speed field instead.
        if (v2_turn_only) {
            strafe_vel_scale = -1.0f;
            scale = vrmod::config.side_turn_speed_scale;
            scale = scale < 0.01f ? 0.01f : (scale > 1.0f ? 1.0f : scale);
        } else {
            float d = sqrtf(stick_raw_side * stick_raw_side +
                            stick_raw_fwd * stick_raw_fwd);
            d = d > 1.0f ? 1.0f : d;
            float fl = vrmod::config.stick_speed_floor;
            fl = fl < 0.01f ? 0.01f : (fl > 1.0f ? 1.0f : fl);
            float vscale = fl + (1.0f - fl) * d * d;
            float bs = vrmod::config.back_speed_scale;
            bs = bs < 0.3f ? 0.3f : (bs > 1.0f ? 1.0f : bs);
            vscale *= 1.0f + (bs - 1.0f) * back_b;
            strafe_vel_scale = vscale;
            strafe_entity = entity;
        }

        // Facing target = facing + commanded turn step.
        const int turn_step = StickTurnStepBams();
        const int turn_apply = hold_turn(turn_step);
        // head_move 2: on movement ticks outside an attack, chase the
        // head's yaw (half the gap, capped 0x800), handed over through
        // follow_pending_* (AbsorbFollowStep). The attack hold's offset
        // also moves into follow_offset at the same rate (view unchanged),
        // so the HUD (facing - hold_offset) comes round to the body.
        int follow_step = 0;
        if (body_follows_head && !v2_turn_only && !action_attacking) {
            const int gap = Wrap16(raw_yaw - (facing + 0x8000));
            follow_step = gap / 2;
            if (follow_step == 0 && gap != 0)
                follow_step = gap > 0 ? 1 : -1;
            if (follow_step > 0x800) follow_step = 0x800;
            if (follow_step < -0x800) follow_step = -0x800;
            if (vrmod::config.attack_view_hold && gamecam::hold_offset != 0) {
                int t = gamecam::hold_offset / 2;
                if (t == 0)
                    t = gamecam::hold_offset > 0 ? 1 : -1;
                if (t > 0x800) t = 0x800;
                if (t < -0x800) t = -0x800;
                gamecam::hold_offset = Wrap16(gamecam::hold_offset - t);
                gamecam::follow_offset = Wrap16(gamecam::follow_offset + t);
            }
        }
        strafe_face_target =
            Wrap16(facing + turn_apply + hold_step + follow_step);
        face_target_bams = strafe_face_target;  // shows in the trace A= column
        gamecam::follow_pending_step = follow_step;
        gamecam::follow_pending_target = strafe_face_target & 0xFFFF;
        // Walk direction. Mode 0: facing-relative, less the attack hold,
        // so stick-forward is the view's forward while re-aligning.
        // Modes 1 / 2: the camera yaw, so forward is where you look.
        steer = head_move != 0 ? raw_yaw
                               : facing + 0x8000 - (hold_attacking ? 0 : gamecam::hold_offset);
    } else {
    strafe_vel_scale = -1.0f;  // stock speeds outside strafe/wind-down
    if (vrmod::config.stick_locomotion)
        stick_turn_scale = -1.0f;  // no legacy controller turn curve in analog mode

    // 1) Reverse detection, from the keys alone. The forward key drives
    // fwd negative (the camera yaw is ~opposite the facing), so the
    // key-relative angle is input + 0x8000.
    bool want_reverse = false;
    if (resolved && vrmod::config.back_strafe && (s != 0 || f != 0)) {
        const int input_bams =
            (int)lroundf(atan2f((float)s, (float)f) * (32768.0f / 3.14159265f));
        const int delta = Wrap16(input_bams + 0x8000);
        const int abs_delta = delta < 0 ? -delta : delta;
        const int threshold = reverse_active ? REVERSE_STAY_BAMS : REVERSE_ENTER_BAMS;
        if (abs_delta >= threshold) {
            want_reverse = true;
            // A = facing + sign(delta) * (0x8000 - |delta|): the 180
            // removed, the left/right sign kept (pure back: A = facing).
            const int residual = (delta >= 0 ? 1 : -1) * (0x8000 - abs_delta);
            face_target_bams = Wrap16(facing + residual);
        }
    }
    reverse_active = want_reverse;

    // 1b) Spot-turn detection: turn-key-only input (fwd exactly 0).
    // Reverse takes priority.
    side_turn_active = resolved && vrmod::config.side_turn && !reverse_active &&
                       s != 0 && f == 0;

    // 2) Steering reference for the patched camera-yaw reads. Spot turn:
    // the carrot (SIDE_TURN_CARROT_BAMS to the pressed side). Otherwise
    // (walking, reversing) anti-facing, so keys steer relative to the
    // character, not the head. The raw camera yaw (stock) when unresolved.
    if (resolved) {
        if (side_turn_active) {
            // Pure-side input angle is +/-0x4000 (s > 0 turns negative);
            // target = input + steer puts it at facing -/+ the carrot for
            // the stand steer and arrived-tick re-aim. MovingTurnStub
            // reads the carrot directly.
            const int sign = s > 0 ? 1 : -1;
            side_turn_target_bams = Wrap16(facing - sign * SIDE_TURN_CARROT_BAMS);
            steer = facing - sign * (0x4000 + SIDE_TURN_CARROT_BAMS);
        } else {
            steer = facing + 0x8000;
        }
    }

    // 3) Speed: reduced walking backwards, collapsed while spot-turning.
    if (reverse_active) {
        scale = vrmod::config.back_speed_scale;
        scale = scale < 0.3f ? 0.3f : (scale > 1.0f ? 1.0f : scale);
    } else if (side_turn_active) {
        scale = vrmod::config.side_turn_speed_scale;
        scale = scale < 0.01f ? 0.01f : (scale > 1.0f ? 1.0f : scale);
    }
    }  // end legacy (keyboard / stick-mix) path

    g_steer_yaw = steer;
    MaintainSpeedScale(entity, scale);

    // Idle re-align (hold_step): nothing else writes the facing now.
    if (hold_step != 0 && !strafe_mode && !kb_had && s == 0 && f == 0 &&
        diag::Accessible(entity + gamecam::ENTITY_FACING_OFFSET, 4, true)) {
        *reinterpret_cast<int32_t*>(entity + gamecam::ENTITY_FACING_OFFSET) =
            (facing + hold_step) & 0xFFFF;
    }

    if (TraceOn())
        TraceInput(entity, s, f, raw_yaw, facing, steer, trace_src);
}

// ---- patch application ---------------------------------------------------

struct Redirect {
    uintptr_t site;   // address of the E8 CALL instruction
    void* stub;
    uint32_t stock_rel;  // saved on first apply
};

inline Redirect redirects[] = {
    {SITE_STAND_STEP, reinterpret_cast<void*>(&StandStepStub), 0},
    {SITE_TURN_RUN, reinterpret_cast<void*>(&MovingTurnStub), 0},
    {SITE_TURN_WALK, reinterpret_cast<void*>(&MovingTurnStub), 0},
    {SITE_MID_RUN, reinterpret_cast<void*>(&MidpointStub), 0},
    {SITE_MID_WALK, reinterpret_cast<void*>(&MidpointStub), 0},
};

// The 6-byte cam-yaw reads -> same-length disp32 forms reading
// &g_steer_yaw (mod=00 rm=101; ADD=03 /r, MOV=8B /r; reg EBP -> 0x2D,
// EBX -> 0x1D, EDX -> 0x15).
struct SitePatch {
    uintptr_t addr;
    uint8_t opcode;
    uint8_t modrm;
    uint8_t saved[6];
    bool have_saved;
};

inline SitePatch camyaw_patches[] = {
    {SITE_CAMYAW_STAND, 0x03, 0x2D, {}, false},
    {SITE_CAMYAW_WALK, 0x03, 0x1D, {}, false},
    {SITE_CAMYAW_RUN, 0x03, 0x1D, {}, false},
    {SITE_CAMYAW_ATTACK, 0x03, 0x15, {}, false},
    {SITE_CAMYAW_PAD, 0x8B, 0x15, {}, false},
};

inline uint32_t applied_rate = 0;  // stand-rate imm currently in the code

inline void WriteBytes(uintptr_t addr, const void* bytes, size_t len) {
    DWORD old_protect;
    if (VirtualProtect(reinterpret_cast<void*>(addr), len, PAGE_EXECUTE_READWRITE, &old_protect)) {
        memcpy(reinterpret_cast<void*>(addr), bytes, len);
        VirtualProtect(reinterpret_cast<void*>(addr), len, old_protect, &old_protect);
    }
}

inline void SetStandStartForced(bool want) {
    if (want == stand_start_forced)
        return;
    const uint8_t b = want ? STAND_ALIGNED_JMP : STAND_ALIGNED_JE_STOCK;
    WriteBytes(STAND_ALIGNED_JE_ADDR, &b, 1);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    stand_start_forced = want;
}

// The standing steer: thiscall on the entity, no stack arguments; its one
// caller 0x006A1B2B returns EAX straight through. Detoured to record the
// ticking entity, and to restore the stock walk-start byte for another
// entity's call.
constexpr uintptr_t STAND_STEER_ADDR = 0x0069B728;
using StandSteerFn = uint32_t(__fastcall*)(void* self, void* edx);
inline StandSteerFn original_stand_steer = nullptr;

inline uint32_t __fastcall StandSteerDetour(void* self, void* edx) {
    TickEntityScope scope(self);
    if (tick_foreign && stand_start_forced) {
        SetStandStartForced(false);
        const uint32_t r = original_stand_steer(self, edx);
        SetStandStartForced(true);
        return r;
    }
    return original_stand_steer(self, edx);
}

inline void InstallInputHook() {
    static bool tried = false;
    if (tried)
        return;
    tried = true;
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        probe::Log("movement: MH_Initialize failed (%d)", (int)status);
        return;
    }
    status = MH_CreateHook(reinterpret_cast<void*>(INPUT_FILL_ADDR),
                           reinterpret_cast<void*>(&InputFillDetour),
                           reinterpret_cast<void**>(&original_input_fill));
    if (status == MH_OK)
        status = MH_EnableHook(reinterpret_cast<void*>(INPUT_FILL_ADDR));
    if (status != MH_OK) {
        probe::Log("movement: input-fill hook failed (%d) - back_strafe unavailable",
                   (int)status);
        original_input_fill = nullptr;
        return;
    }
    probe::Log("movement: input-fill detour installed at 0x%08X",
               (unsigned)INPUT_FILL_ADDR);

    // Analog speed (ScaledSpeedGuard). A failed hook leaves stock speed.
    status = MH_CreateHook(reinterpret_cast<void*>(RUN_VEL_BUILD_ADDR),
                           reinterpret_cast<void*>(&RunVelBuildDetour),
                           reinterpret_cast<void**>(&original_run_vel));
    if (status == MH_OK)
        status = MH_EnableHook(reinterpret_cast<void*>(RUN_VEL_BUILD_ADDR));
    if (status != MH_OK) {
        probe::Log("movement: run velocity-build hook failed (%d)", (int)status);
        original_run_vel = nullptr;
    }
    status = MH_CreateHook(reinterpret_cast<void*>(WALK_VEL_BUILD_ADDR),
                           reinterpret_cast<void*>(&WalkVelBuildDetour),
                           reinterpret_cast<void**>(&original_walk_vel));
    if (status == MH_OK)
        status = MH_EnableHook(reinterpret_cast<void*>(WALK_VEL_BUILD_ADDR));
    if (status != MH_OK) {
        probe::Log("movement: walk velocity-build hook failed (%d)", (int)status);
        original_walk_vel = nullptr;
    }
    status = MH_CreateHook(reinterpret_cast<void*>(FACING_TICK_RUN_ADDR),
                           reinterpret_cast<void*>(&RunFacingTickDetour),
                           reinterpret_cast<void**>(&original_run_facing_tick));
    if (status == MH_OK)
        status = MH_EnableHook(reinterpret_cast<void*>(FACING_TICK_RUN_ADDR));
    if (status != MH_OK) {
        probe::Log("movement: run facing-tick hook failed (%d)", (int)status);
        original_run_facing_tick = nullptr;
    }
    status = MH_CreateHook(reinterpret_cast<void*>(FACING_TICK_WALK_ADDR),
                           reinterpret_cast<void*>(&WalkFacingTickDetour),
                           reinterpret_cast<void**>(&original_walk_facing_tick));
    if (status == MH_OK)
        status = MH_EnableHook(reinterpret_cast<void*>(FACING_TICK_WALK_ADDR));
    if (status != MH_OK) {
        probe::Log("movement: walk facing-tick hook failed (%d)", (int)status);
        original_walk_facing_tick = nullptr;
    }
    if (original_run_vel != nullptr && original_walk_vel != nullptr &&
        original_run_facing_tick != nullptr && original_walk_facing_tick != nullptr)
        probe::Log("movement: velocity write-site hooks installed (analog speed)");
    // Entity guard on the standing steer (the facing ticks have theirs).
    status = MH_CreateHook(reinterpret_cast<void*>(STAND_STEER_ADDR),
                           reinterpret_cast<void*>(&StandSteerDetour),
                           reinterpret_cast<void**>(&original_stand_steer));
    if (status == MH_OK)
        status = MH_EnableHook(reinterpret_cast<void*>(STAND_STEER_ADDR));
    if (status != MH_OK) {
        probe::Log("movement: standing-steer hook failed (%d) - other entities may take our facing in stand mode",
                   (int)status);
        original_stand_steer = nullptr;
    } else {
        probe::Log("movement: entity guard installed (standing steer + facing ticks steer only our entity)");
    }
}

inline void ApplyRedirects() {
    for (Redirect& r : redirects) {
        if (r.stock_rel == 0)
            memcpy(&r.stock_rel, reinterpret_cast<void*>(r.site + 1), 4);
        const uint32_t rel = (uint32_t)(reinterpret_cast<uintptr_t>(r.stub) - (r.site + 5));
        WriteBytes(r.site + 1, &rel, 4);
    }
    for (SitePatch& p : camyaw_patches) {
        if (!p.have_saved) {
            memcpy(p.saved, reinterpret_cast<void*>(p.addr), 6);
            p.have_saved = true;
        }
        uint8_t code[6] = {p.opcode, p.modrm, 0, 0, 0, 0};
        const uintptr_t target = reinterpret_cast<uintptr_t>(&g_steer_yaw);
        memcpy(code + 2, &target, 4);
        WriteBytes(p.addr, code, 6);
    }
}

inline void RevertRedirects() {
    for (Redirect& r : redirects)
        if (r.stock_rel != 0)
            WriteBytes(r.site + 1, &r.stock_rel, 4);
    for (SitePatch& p : camyaw_patches)
        if (p.have_saved)
            WriteBytes(p.addr, p.saved, 6);
}

// Called every frame from BeginScene. Applies or reverts the patches on
// state change and keeps the stand-rate immediate at the current scale.
inline void OnFrame(bool takeover_driving) {
    bool wrote = false;
    if (takeover_driving && !patches_applied) {
        InstallInputHook();  // detour stays installed; gates on patches_applied
        ApplyRedirects();
        patches_applied = true;
        wrote = true;
        probe::Log("movement: locomotion patches applied (turnscale=%.2f backstrafe=%d)",
                   Scale(), vrmod::config.back_strafe ? 1 : 0);
    } else if (!takeover_driving && patches_applied) {
        RevertRedirects();
        const uint32_t stock = STAND_TURN_RATE_STOCK;
        WriteBytes(STAND_TURN_RATE_IMM_ADDR, &stock, 4);
        patches_applied = false;
        applied_rate = 0;
        reverse_active = false;
        side_turn_active = false;
        strafe_mode = false;
        strafe_vel_scale = -1.0f;
        strafe_winddown_ticks = 0;
        SetStandStartForced(false);
        wrote = true;
        probe::Log("movement: locomotion patches reverted");
    }
    if (patches_applied) {
        uint32_t want_rate = (uint32_t)(STAND_TURN_RATE_STOCK * Scale());
        if (want_rate < 1)
            want_rate = 1;
        if (want_rate != applied_rate) {
            WriteBytes(STAND_TURN_RATE_IMM_ADDR, &want_rate, 4);
            applied_rate = want_rate;
            wrote = true;
        }
    }
    if (wrote)
        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
}

}  // namespace movement
