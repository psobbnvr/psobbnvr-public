#pragma once

// Guns: bullets leave the controller-held barrel instead of the body,
// free fire follows the barrel, per-hand dual mechguns, and the attack
// facing snap suppression with a gun.
//
// The per-bullet spawn has three siblings, all thiscall on the weapon,
// all ending in the projectile-spawn callback
// [weapon+0x238](weapon, source, dest, ticks):
//   0x005E6578  (vec3* source, int pair_index, x, int do_damage), ret 0x10
//     - the base weapon virtual (vtbl+0xC8). Classes whose fire override
//       (vtbl+0xCC) is the no-op stub 0x61CDB0 (plain guns, autogun)
//       fire only through this, at hit pair[pair_index] (0-based) with
//       velocity lead. The mechgun overrides (cat 0x08 0x5F4620, cat
//       0x6C Yasminkov 0x6018AC) call it twice per burst bullet at pair
//       0, sourced from the weapon's two barrel vec3s (+0x240/+0x24C,
//       Yasminkov +0x250/+0x25C); the second call has do_damage 0.
//   0x005E6A88  (float mult, vec3* source, int do_damage), ret 0xC
//     - wrapper used by several overrides; source 0 = the owner's
//       position (entity+0x300), one family passes a model point
//       (0x5F048C via 0x5E7534/0x5E83C8).
//   0x005E7014  (vec3* source, u32 param, int yaw_offset), ret 0xC
//     - the target-leading wrapper (rare-gun families; spreads call it
//       per bullet with different yaw offsets).
// The game's source is animation-derived. Muzzle flash (+0x234
// callback), the bullet (+0x238: effect 9 flying source -> dest over
// the given ticks) and the scheduled impacts all key off source/dest.
//
// For the local player's held gun, per shot:
// - source becomes the right hand's barrel ray (weapongrip::BarrelRay)
//   plus gun_muzzle_offset_m along it.
// - Targeted shots need nothing else: dest is the target's part
//   position, so the bullet flies barrel -> enemy.
// - Untargeted shots: during the original call the weapon's +0x238
//   callback points at a shim that replaces dest with muzzle + aim ray x
//   the weapon's fallback range (ticks recomputed, wall raycast redone
//   on our line). Sounds, gates and damage stay native.
// Hit resolution is unchanged; only where the visual starts and where a
// missed or unlocked bullet flies move.
//
// Known gaps: the recoil effect (0x814298 id 100 at entity+0x38) stays on
// the character; some families may fire untargeted through other routes
// (0x5E90E8 -> entity virtual +0x114), which the first-call logs would
// show.
//
// Config [vr] gun_fire_origin, gun_muzzle_offset_m. The first shot
// through each path always logs.

#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "MinHook.h"
#include "psobbvr_gamecam.hpp"
#include "psobbvr_log.hpp"
#include "psobbvr_probe.hpp"
#include "psobbvr_targetaim.hpp"
#include "psobbvr_vr.hpp"
#include "psobbvr_weapongrip.hpp"

namespace gunfire {

// The fire siblings and their helpers.
constexpr uintptr_t SHOT_FIRE0_ADDR = 0x005E6578;  // variant 0: base virtual
constexpr uintptr_t SHOT_FIRE_ADDR = 0x005E6A88;   // variant 1
constexpr uintptr_t SHOT_FIRE2_ADDR = 0x005E7014;  // variant 2 (leading)
// Variant 3: the launcher family's fire body 0x5E6DF8(vec3* source, u32
// param, int yaw_offset), ret 0xC, reached from the vtbl+0xCC override
// 0x5EB1A8. It skips the +0x238 callback and fills a spawn descriptor
// on its stack: pos = source; yaw +0x28 = [owner+0x60] + yaw_offset;
// speed +0x38 = [weapon+0x208]; range +0x3C; flags +0x40 (bit 0 = use
// the direction vector at +0x2C, set by the locked branch toward pair
// 0's part; 0x800 = wall between body and muzzle, pos moved to it).
// The class virtual vtbl+0xE8 (0x5EB1DC for launchers) adds the
// explosion, then 0x5C60F0 spawns the shell (ctor 0x5C6164: velocity =
// dir * speed). Unlocked, the direction is the body facing.
constexpr uintptr_t SHOT_FIRE3_ADDR = 0x005E6DF8;  // variant 3 (launcher family)
// The projectile spawner variant 3 calls, cdecl(mgr, desc) -> object.
// Hooked so our unlocked launcher shot sets the descriptor's direction
// vector (flags bit 0, +0x2C) to the aim ray, as the locked branch does
// toward its target; the ctor takes yaw and pitch from it.
constexpr uintptr_t SHELL_SPAWN_ADDR = 0x005C60F0;
constexpr uintptr_t DESC_DIR_OFF = 0x2C;
constexpr uintptr_t DESC_FLAGS_OFF = 0x40;
// cdecl (vec3* from, vec3* to, int mask) -> collision record or 0 (see
// PrepareFreeFire). Mask 0x8100 = walls, as the game's shot path uses.
constexpr uintptr_t WALL_CLIP_ADDR = 0x0077FFFC;
// cdecl (weapon, int idx) -> float*; idx 2's [0] = the no-target range.
constexpr uintptr_t RANGE_GETTER_ADDR = 0x005E4BA8;
// cdecl (u16 entity_id, int part) -> the target entity or 0: the id
// resolves, the entity is alive and not flagged 0x4040800 at +0x30, the
// part record ([entity+0xA4] + part*0x2C) has +0x18 bit 0 set, +0x96 ==
// 1. The same test the fire routines' targeted branches run. It returns
// the entity, not the part: the part position is [entity+0xA4] +
// part*0x2C + 0x1C (as 0x6A06A8 computes it).
constexpr uintptr_t PART_LOOKUP_ADDR = 0x007B9DF0;
constexpr uintptr_t ENT_PART_ARRAY_OFF = 0xA4;   // ptr: part records
constexpr uintptr_t ENT_PART_STRIDE = 0x2C;
// Attack-start facing snap threshold: the attack dispatch turns the
// character toward the target when its distance >= this float (stock
// 0.10, so always). Its only readers are the four snap sites (three in
// 0x68A40C, one in 0x68C190). Held at +huge while a gun is held with
// gun_facing_snap=0 (cosmetic: bullets home and free fire uses our ray).
// Melee attacks keep the snap: without the turn the attack step misses.
constexpr uintptr_t SNAP_THRESHOLD_ADDR = 0x0093C754;
// Per-tick facing chase: the attack stage machine 0x698FA8 (thiscall on
// the entity, no args) starts each tick by stepping the facing toward
// [entity+0x3FC] (0x7A8CB8), independent of the snap threshold.
// Suppressed by setting +0x3FC to the current facing for the call.
constexpr uintptr_t STAGE_MACHINE_ADDR = 0x00698FA8;
constexpr uintptr_t ENT_TARGET_YAW_OFF = 0x3FC;
constexpr uintptr_t ENT_FACING_OFF = 0x60;
// Melee attacks are tracked in the stage-machine hook (a call gap over
// MELEE_SNAP_GAP_S or a combo step change = a new attack), for the
// chained-step re-target below. An attack is ours when our swing fired
// it within MELEE_SNAP_OURS_S. Each attack logs one 'meleesnap:' line
// with the start turn and the chase total.
constexpr double MELEE_SNAP_OURS_S = 0.5;
constexpr double MELEE_SNAP_GAP_S = 0.3;
constexpr uintptr_t ENT_ATK_VARIANT_WORD_OFF = 0x8A4;  // word: 0/1/2 = n/h/s
constexpr uintptr_t ENT_COMBO_STEP_OFF = 0x8B4;        // int: combo step
// Combo re-target ([vr] attack_retarget): the targeting request
// (+0x1098/+0x109A) pins the target for the whole attack, so a chained
// combo step would hit the first swing's enemy wherever the head now
// points. On a chained step (+0x8B4 changes within one attack) the
// request is rewritten to the nearest attack-bank candidate
// (targetaim::aim_cands) within retarget_cone_deg of the gaze, and the
// character is turned to it (+0x3FC and +0x60; the view hold absorbs
// the turn). The yaw runs from the position block +0x38 to the part
// position via the game's helper 0x7A90A8 (atan2(dx, dz) in BAMS).
constexpr uintptr_t YAW_TO_POINT_ADDR = 0x007A90A8;  // cdecl(vec3* from, vec3* to) -> BAMS
using YawToPointFn = int(__cdecl*)(const float* from, const float* to);
constexpr uintptr_t ENT_POS_BLOCK_OFF = 0x38;
constexpr uintptr_t PART_POS_OFF = 0x1C;
constexpr uintptr_t TGT_REQ_CODE_OFF = 0x109C;
// cdecl (mgr, weapon, vec3* source, vec3* hitpos, int ticks): the
// wall-impact scheduler variant 0 calls from its own wall raycast along
// the character's line. Suppressed during our free fire; PrepareFreeFire
// spawns the impact on our ray instead.
constexpr uintptr_t WALL_IMPACT_ADDR = 0x005CF0D0;
constexpr uintptr_t WEAPON_OWNER_OFF = 0xF8;    // weapon -> owner entity
constexpr uintptr_t WEAPON_SPEED_OFF = 0x208;   // float bullet speed
constexpr uintptr_t WEAPON_PROJ_CB_OFF = 0x238; // projectile-spawn callback
constexpr uintptr_t ENT_HIT_INDEX_OFF = 0x8D0;  // hit-pair index; id/part
                                                // pairs at +idx*4/+idx*4+2

// ---- Per-hand mechguns. The mechgun fire overrides call the base
// virtual once per barrel per burst bullet, each sourced from a barrel
// vec3 on the weapon. Wrapping them lets us write our hand muzzles into
// those slots, tell each call's hand by its source pointer, fire the
// left barrel at the left-hand lock, and halve each barrel's damage
// (full damage needs both hands on the same enemy and part).
constexpr uintptr_t MECH_FIRE_CAT8_ADDR = 0x005F4620;  // cat 0x08 override
constexpr uintptr_t MECH_FIRE_YAS_ADDR = 0x006018AC;   // cat 0x6C Yasminkov
constexpr uintptr_t MECH_FIRE_BASE_ADDR = 0x005EC2BC;  // shared dual base
                                                       // (vtable 0xB11920)
// The per-hit filler: fills the +0x8D0 pair block and sends subcommand
// 0x46 (the hit report). Hooked so a split hit also reports the left
// victim, keeping the server's accounting in step with local damage.
constexpr uintptr_t HIT_REPORT_ADDR = 0x00699248;
// Damage-number popup constructor (cdecl: vec3* position, int colour
// (-1 = default), int kind (0x14 for damage), int number). The applier
// 0x7732B8 calls it at 0x773863 and ignores the return value, so a held
// popup can return null. Both halves of a dual-mechgun bullet land on
// the same enemy within a tick and would draw as two identical numbers;
// the hook holds the first and adds the second into it.
constexpr uintptr_t POPUP_CTOR_ADDR = 0x0078A5E8;
// Two popups merge only within this distance (game units) of each
// other: the same body part = the same hit position.
constexpr float POPUP_MERGE_RADIUS = 2.0f;
constexpr uintptr_t ENT_ID_WORD_OFF = 0x1C;  // word entity id
// The reticle object (0x68 bytes, vtable 0xB45F30): vtbl[2] = the draw
// 0x7A356C; 0x7A360C renders the billboard at the world position at
// +0x40; 0x829CC8 (ecx = 0x9B97A8) is the texture/state bind before the
// render. The left-hand reticle renders the game's own object at another
// position, rotated 180 degrees, so two on one target form a six-point
// star.
constexpr uintptr_t RETICLE_DRAW_ADDR = 0x007A356C;
constexpr uintptr_t RETICLE_RENDER_ADDR = 0x007A360C;
constexpr uintptr_t RETICLE_REFRESH_ADDR = 0x007A35C8;  // re-pull target pos -> +0x40
constexpr uintptr_t RETICLE_BIND_ADDR = 0x00829CC8;
constexpr uintptr_t RETICLE_BIND_OBJ = 0x009B97A8;
// The render's three triangles use fixed angle immediates 0x2AAA /
// 0x7FFF / 0xD554 (60/180/300 deg BAMS) fed to the rotate helper
// 0x830C04; the left copy patches them +0x8000 around its render call.
// (+0x30 is a hover height, not a spin phase.)
constexpr uintptr_t RETICLE_ANGLE_ADDRS[3] = {0x007A3657, 0x007A369B,
                                              0x007A36D8};
constexpr uint32_t RETICLE_ANGLE_BASE[3] = {0x2AAA, 0x7FFF, 0xD554};
// cdecl (word id) -> entity ptr or 0 (the game's own by-ID lookup).
constexpr uintptr_t ENTITY_BY_ID_ADDR = 0x007B4D18;
constexpr uintptr_t ENT_PARTS_OFF = 0xA4;  // stride-0x2C part records;
                                           // position at +0x1C/20/24
constexpr uintptr_t ENT_BURST_OFF = 0x8B8; // kind-8 burst hit counter
                                           // (reset at attack start)

using StageMachineFn = void(__fastcall*)(void* entity, void* edx);
using MechFireFn = void(__fastcall*)(void* weapon, void* edx);
using HitReportFn = void(__fastcall*)(void* entity, void* edx);
using ReticleFn = void(__fastcall*)(void* obj, void* edx);
using EntityByIdFn = void*(__cdecl*)(int id);
using ShotFire0Fn = int(__fastcall*)(void* weapon, void* edx, float* source,
                                     int pair_index, uint32_t param,
                                     int do_damage);
using ShotFireFn = void(__fastcall*)(void* weapon, void* edx, float mult,
                                     float* source, int do_damage);
using ShotFire2Fn = void(__fastcall*)(void* weapon, void* edx, float* source,
                                      uint32_t param, int yaw_offset);
using ProjSpawnFn = void(__cdecl*)(void* weapon, float* source, float* dest,
                                   float ticks);
using WallClipFn = int(__cdecl*)(const float* from, const float* to,
                                 int mask);
using WallImpactFn = int(__cdecl*)(void* mgr, void* weapon, float* source,
                                   float* hitpos, int ticks);
using RangeGetFn = float*(__cdecl*)(void* weapon, int idx);
using PartLookupFn = int(__cdecl*)(uint16_t id, int part);

inline StageMachineFn original_stage = nullptr;
inline ShotFire0Fn original0 = nullptr;
inline ShotFireFn original = nullptr;
inline ShotFire2Fn original2 = nullptr;
using ShotFire3Fn = void(__fastcall*)(void* weapon, void* edx, float* source,
                                      uint32_t param, int yaw_offset);
inline ShotFire3Fn original3 = nullptr;
inline bool logged_v3_call = false;
using ShellSpawnFn = void*(__cdecl*)(void* mgr, float* desc);
inline ShellSpawnFn original_spawn = nullptr;
inline bool v3_window = false;   // inside our unlocked variant-3 call
inline bool logged_v3_spawn = false;
inline WallImpactFn original_impact = nullptr;
inline bool installed = false;

// Aim rays per hand (0 left, 1 right) in world units, computed once per
// frame in OnFrame. aim_valid is the right hand's; without a left pose
// the dual-mechgun mode stays off.
inline bool aim_valid = false;
inline bool aim_valid_l = false;
inline float aim_origin[2][3] = {};
inline float aim_fwd[2][3] = {};
inline int ff_hand = 1;        // hand chosen for the shot in flight
inline bool logged_ray_source = false;

// Dual-mechgun state. Detected per frame from the held weapon's
// vtbl+0xCC fire override (MinHook patches code, so the vtable still
// holds the original addresses).
inline int dual_kind = -1;         // -1 off; 0 cat8, 1 Yasminkov, 2 base
inline uintptr_t dual_slot_a = 0;  // barrel vec3 offsets on the weapon
inline uintptr_t dual_slot_b = 0;  // (slot A = the do_damage-first call)
// The left lock, mirrored from targetaim::dual by SyncLocks. The right
// set (targetaim::RightSees) decides which hand owns the game's target,
// which may have been fed by the left hand.
inline bool left_lock_valid = false;
inline uint16_t left_lock_id = 0xFFFF;
inline int16_t left_lock_part = 0;
// Split state: computed when the dual window opens and again in the
// hit-report hook.
inline bool hit_split = false;
inline bool hit_same = false;
inline bool hit_right_owns = false;  // the right ray sees pair 0's enemy
inline bool hit_left_owns = false;   // pair 0's enemy IS the left lock's
inline bool hit_left_ok = false;     // the left lock validated this hit
inline uint16_t hit_left_id = 0xFFFF;
inline int16_t hit_left_part = 0;
inline uint16_t hit_right_id = 0xFFFF;
inline int16_t hit_right_part = 0;
inline void SyncLocks() {
    const targetaim::DualState& d = targetaim::dual;
    left_lock_valid = dual_kind >= 0 && d.valid && d.active && d.left_valid;
    left_lock_id = left_lock_valid ? d.left[0].id : 0xFFFF;
    left_lock_part = left_lock_valid ? d.left[0].part : 0;
}
// Half damage: the fire virtual copies the variant damage multiplier
// 0x9CB9D0[entity+0x8A4] into the scheduled impact (+0x14 -> impact
// +0x30). The arrival callback 0x5C7110 passes it via weapon vtbl+0xF8
// (0x5E911C) to the applier (0x7732B8), which applies it at 0x7736C6
// after the DFP subtraction, so half the multiplier is half the final
// damage (popup, HP and server sync follow). It is held at half around
// every targeted dual-window call. Normal/heavy only; specials ignore it.
constexpr uintptr_t VARIANT_DMG_TABLE_ADDR = 0x009CB9D0;
constexpr uintptr_t ENT_ATTACK_VARIANT_OFF = 0x8A4;
// Set during one wrapped override call (its base-virtual calls land in
// Hook0).
inline bool dual_window = false;
inline void* dual_weapon = nullptr;
inline MechFireFn original_mech0 = nullptr;  // cat 8
inline MechFireFn original_mech1 = nullptr;  // Yasminkov
inline MechFireFn original_mech2 = nullptr;  // shared dual base
inline HitReportFn original_report = nullptr;
using PopupCtorFn = void*(__cdecl*)(const float* pos, int colour, int kind,
                                    int number);
inline PopupCtorFn original_popup = nullptr;
inline bool popup_hook_ok = false;
// The held popup (mechgun_merge_popups): flushed from OnFrame two frames
// after it was held, or at once when another popup arrives.
inline bool pending_popup = false;
inline bool pending_merged = false;
inline float pending_pos[3] = {};
inline int pending_colour = -1;
inline int pending_kind = 0;
inline int pending_number = 0;
inline uintptr_t pending_victim = 0;
inline unsigned pending_tick = 0;
inline unsigned popup_tick = 0;  // counts OnFrame calls
inline ReticleFn original_reticle = nullptr;
inline bool mech_hooks_ok = false;
inline bool reticle_hook_ok = false;
inline bool logged_mech_call = false;

// The first call through each variant always logs, before any gating.
inline bool logged_v0_call = false;
inline bool logged_v1_call = false;
inline bool logged_v2_call = false;
inline bool logged_first_targeted = false;
inline bool logged_first_free = false;

// Valid only for the duration of one swapped original call.
inline ProjSpawnFn real_cb = nullptr;

// Free-fire state, set before the original call and valid only inside
// it: dest on the aim ray, the wall point on our line, travel ticks.
inline bool ff_active = false;
inline float ff_dest[3] = {};
inline bool ff_wall_ok = false;
inline float ff_wall[3] = {};
inline float ff_ticks = 0.0f;

// yaw_bams: the base virtual's yaw-offset argument, added to the facing
// for untargeted shots; the shot family fans five pellets with it (0,
// +-12, +-25 degrees, table 0x92DFFC). Applied about world up so the fan
// follows the controller ray.
inline void PrepareFreeFire(void* weapon, const float* muzzle,
                            int32_t yaw_bams = 0) {
    float range = 60.0f;  // fallback if the getter misbehaves
    const float* r =
        reinterpret_cast<RangeGetFn>(RANGE_GETTER_ADDR)(weapon, 2);
    if (r != nullptr && r[0] > 1.0f && r[0] < 10000.0f)
        range = r[0];
    float fwd[3] = {aim_fwd[ff_hand][0], aim_fwd[ff_hand][1],
                    aim_fwd[ff_hand][2]};
    if (yaw_bams != 0) {
        const float a = (float)yaw_bams * (6.28318531f / 65536.0f);
        const float c = cosf(a), s = sinf(a);
        fwd[0] = aim_fwd[ff_hand][0] * c + aim_fwd[ff_hand][2] * s;
        fwd[2] = -aim_fwd[ff_hand][0] * s + aim_fwd[ff_hand][2] * c;
    }
    ff_dest[0] = muzzle[0] + fwd[0] * range;
    ff_dest[1] = muzzle[1] + fwd[1] * range;
    ff_dest[2] = muzzle[2] + fwd[2] * range;
    float dist = range;
    uint32_t surf_flags = 0;
    const int rec = reinterpret_cast<WallClipFn>(WALL_CLIP_ADDR)(
        muzzle, ff_dest, 0x8100);
    // The record holds a pointer at +4 to the hit surface (position at
    // +0, flags dword at +0x18), as 0x5E6BB0 reads it.
    const float* hit =
        rec != 0 ? *reinterpret_cast<const float* const*>(rec + 4) : nullptr;
    ff_wall_ok = hit != nullptr;
    if (ff_wall_ok) {
        ff_wall[0] = hit[0];
        ff_wall[1] = hit[1];
        ff_wall[2] = hit[2];
        surf_flags = *reinterpret_cast<const uint32_t*>(
            reinterpret_cast<uintptr_t>(hit) + 0x18);
        const float dx = hit[0] - muzzle[0];
        const float dy = hit[1] - muzzle[1];
        const float dz = hit[2] - muzzle[2];
        dist = sqrtf(dx * dx + dy * dy + dz * dz);
        // As variant 0 does: dest moves to the wall so the bullet keeps
        // its speed and dies there.
        ff_dest[0] = hit[0];
        ff_dest[1] = hit[1];
        ff_dest[2] = hit[2];
    }
    ff_ticks = 0.0f;
    const float speed =
        *reinterpret_cast<const float*>((uintptr_t)weapon + WEAPON_SPEED_OFF);
    if (speed > 0.001f)
        ff_ticks = floorf(dist / speed + 0.5f);
    if (ff_ticks < 0.0f)
        ff_ticks = 0.0f;
    ff_active = true;
    // Spawn the wall impact on our ray (the original's raycast follows
    // the character's line and often misses). Same gates as variant 0:
    // flag-0x100 surfaces, weapon has a muzzle-flash callback. Through
    // the trampoline so our suppressing hook lets it pass.
    if (ff_wall_ok && (surf_flags & 0x100) != 0 &&
        *reinterpret_cast<void* const*>((uintptr_t)weapon + 0x234) !=
            nullptr &&
        original_impact != nullptr) {
        void* mgr = *reinterpret_cast<void* const*>(0x00ACA36C);
        if (mgr != nullptr)
            original_impact(mgr, weapon, const_cast<float*>(muzzle), ff_wall,
                            (int)ff_ticks);
    }
}

// Facing-snap hold state (see SNAP_THRESHOLD_ADDR).
inline float snap_saved = 0.0f;
inline bool snap_saved_valid = false;
inline bool snap_patched = false;

inline void WriteSnapThreshold(float value) {
    DWORD old_protect = 0;
    void* addr = reinterpret_cast<void*>(SNAP_THRESHOLD_ADDR);
    if (VirtualProtect(addr, 4, PAGE_READWRITE, &old_protect)) {
        *reinterpret_cast<float*>(addr) = value;
        VirtualProtect(addr, 4, old_protect, &old_protect);
    }
}

inline bool SnapSuppressed() {
    return installed && !vrmod::config.gun_facing_snap &&
           vrmod::config.weapon_grip && gamecam::DrivesView() &&
           weapongrip::current_is_gun;
}

// Melee attack tracking state (see MELEE_SNAP_OURS_S).
inline LONGLONG ms_qpf = 0;
inline LONGLONG ms_fire_qpc = 0;      // last swing-fired normal press
inline int32_t ms_fire_facing = 0;    // facing (BAMS) at that call
inline bool ms_in_attack = false;     // the hook is tracking an attack
inline bool ms_ours = false;          // ... started by our fire
inline int ms_variant = 0;
inline int ms_step = -1;
inline LONGLONG ms_last_call_qpc = 0;
inline double ms_whip_deg = 0.0;
inline double ms_chase_deg = 0.0;
inline int ms_ticks = 0;

inline LONGLONG MsNow() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    if (ms_qpf == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        ms_qpf = f.QuadPart;
    }
    return q.QuadPart;
}
inline double MsSeconds(LONGLONG from, LONGLONG to) {
    return ms_qpf > 0 ? double(to - from) / double(ms_qpf) : 1e9;
}
// Signed shortest turn a -> b in degrees (facing is a 16-bit BAMS angle
// widened to an int).
inline double BamsDeltaDeg(int32_t a, int32_t b) {
    const int16_t d = static_cast<int16_t>(static_cast<uint32_t>(b - a));
    return d * (360.0 / 65536.0);
}

inline void MeleeSnapReport() {
    if (!ms_in_attack)
        return;
    ms_in_attack = false;
    diag::Log("meleesnap: variant %d step %d %s: whip %+.1f deg at start, "
              "chase %.1f deg over %d ticks",
              ms_variant, ms_step, ms_ours ? "ours" : "native",
              ms_whip_deg, ms_chase_deg, ms_ticks);
}

inline void MaintainFacingSnap() {
    if (SnapSuppressed()) {
        if (!snap_patched) {
            if (!snap_saved_valid) {
                snap_saved =
                    *reinterpret_cast<const float*>(SNAP_THRESHOLD_ADDR);
                snap_saved_valid = true;
            }
            WriteSnapThreshold(1e30f);
            snap_patched = true;
            diag::Log("gunsnap: hold engaged (threshold was %.2f)",
                      snap_saved);
        }
    } else if (snap_patched) {
        WriteSnapThreshold(snap_saved);
        snap_patched = false;
        diag::Log("gunsnap: hold released");
    }
    // Report an attack the machine has stopped ticking.
    if (ms_in_attack && MsSeconds(ms_last_call_qpc, MsNow()) > MELEE_SNAP_GAP_S)
        MeleeSnapReport();
}

// Called when the swing detector fires a normal attack: stamp it and note
// the facing.
inline void MeleeSnapFire() {
    ms_fire_qpc = MsNow();
    const uintptr_t e = gamecam::ResolveEntity();
    ms_fire_facing =
        e != 0 ? *reinterpret_cast<const int32_t*>(e + ENT_FACING_OFF) : 0;
}

// The chained-step re-target (see YAW_TO_POINT_ADDR), melee only. The
// melee bank is omnidirectional, so the gaze cone is applied here; with
// no candidate in the cone the lock stays.
inline void RetargetChainedStep(uintptr_t e) {
    if (weapongrip::current_is_gun || targetaim::aim_cands_n == 0 ||
        targetaim::aim_cands_source != 'M')
        return;
    const uintptr_t tgt =
        *reinterpret_cast<const uintptr_t*>(e + targetaim::ENT_TARGETING_OFF);
    if (tgt == 0 || !diag::Accessible(tgt + targetaim::TGT_REQ_ID_OFF, 8, true))
        return;
    const float cone = vrmod::config.retarget_cone_deg;
    const int32_t gaze = (int32_t)targetaim::aim_yaw_bams;
    const float* from = reinterpret_cast<const float*>(e + ENT_POS_BLOCK_OFF);
    int pick = -1, pick_yaw = 0;
    float pick_off = 0.0f;
    for (int i = 0; i < targetaim::aim_cands_n; i++) {
        const targetaim::Cand& c = targetaim::aim_cands[i];
        const uintptr_t ent = (uintptr_t)reinterpret_cast<PartLookupFn>(
            PART_LOOKUP_ADDR)(c.id, (int)c.part);
        if (ent == 0 || c.part < 0)
            continue;
        const uintptr_t parts =
            *reinterpret_cast<const uintptr_t*>(ent + ENT_PART_ARRAY_OFF);
        if (parts == 0)
            continue;
        const uintptr_t pos =
            parts + (uintptr_t)c.part * ENT_PART_STRIDE + PART_POS_OFF;
        if (!diag::Accessible(pos, 12, false))
            continue;
        const int yaw = reinterpret_cast<YawToPointFn>(YAW_TO_POINT_ADDR)(
            from, reinterpret_cast<const float*>(pos));
        const float off = (float)BamsDeltaDeg(gaze, yaw & 0xFFFF);
        if (fabsf(off) <= cone) {
            pick = i;
            pick_yaw = yaw & 0xFFFF;
            pick_off = off;
            break;  // nearest first
        }
    }
    if (pick < 0)
        return;
    const targetaim::Cand c = targetaim::aim_cands[pick];
    uint16_t* req_id = reinterpret_cast<uint16_t*>(tgt + targetaim::TGT_REQ_ID_OFF);
    int16_t* req_part = reinterpret_cast<int16_t*>(tgt + targetaim::TGT_REQ_PART_OFF);
    if (*req_id == c.id && *req_part == c.part)
        return;
    const uint16_t old_id = *req_id;
    *req_id = c.id;
    *req_part = c.part;
    *reinterpret_cast<int32_t*>(tgt + TGT_REQ_CODE_OFF) = 1;
    const int32_t before = *reinterpret_cast<const int32_t*>(e + ENT_FACING_OFF);
    *reinterpret_cast<int32_t*>(e + ENT_TARGET_YAW_OFF) = pick_yaw;
    *reinterpret_cast<int32_t*>(e + ENT_FACING_OFF) = pick_yaw;
    diag::Log("retarget: chained step -> %04X/%d (was %04X; %.1f away, "
              "%+.1f deg off the gaze, %d candidates), turned %+.1f deg",
              c.id, c.part, old_id, sqrtf(c.d2), pick_off,
              targetaim::aim_cands_n, BamsDeltaDeg(before, pick_yaw));
}

// The chase suppressor (see STAGE_MACHINE_ADDR), local player only: every
// tick while a gun is held. Melee attacks are tracked for the chained-step
// re-target and measured for the 'meleesnap:' line.
inline void __fastcall StageMachineHook(void* entity, void* edx) {
    const uintptr_t e = reinterpret_cast<uintptr_t>(entity);
    if (e == 0 || e != gamecam::ResolveEntity()) {
        original_stage(entity, edx);
        return;
    }
    int32_t* target_yaw = reinterpret_cast<int32_t*>(e + ENT_TARGET_YAW_OFF);
    const int32_t* facing = reinterpret_cast<const int32_t*>(e + ENT_FACING_OFF);
    if (weapongrip::current_is_gun) {
        if (!SnapSuppressed()) {
            original_stage(entity, edx);
            return;
        }
        const int32_t saved = *target_yaw;
        *target_yaw = *facing;
        original_stage(entity, edx);
        *target_yaw = saved;
        return;
    }
    const LONGLONG now = MsNow();
    const int variant =
        *reinterpret_cast<const int16_t*>(e + ENT_ATK_VARIANT_WORD_OFF);
    const int step = *reinterpret_cast<const int32_t*>(e + ENT_COMBO_STEP_OFF);
    const bool chained =
        ms_in_attack && step != ms_step &&
        MsSeconds(ms_last_call_qpc, now) <= MELEE_SNAP_GAP_S;
    if (chained && vrmod::config.attack_retarget)
        RetargetChainedStep(e);
    if (!ms_in_attack || step != ms_step ||
        MsSeconds(ms_last_call_qpc, now) > MELEE_SNAP_GAP_S) {
        MeleeSnapReport();
        ms_in_attack = true;
        ms_step = step;
        ms_variant = variant;
        ms_ours = ms_fire_qpc != 0 &&
                  MsSeconds(ms_fire_qpc, now) <= MELEE_SNAP_OURS_S;
        ms_whip_deg = ms_ours ? BamsDeltaDeg(ms_fire_facing, *facing) : 0.0;
        ms_chase_deg = 0.0;
        ms_ticks = 0;
    }
    ms_last_call_qpc = now;
    const int32_t before = *facing;
    original_stage(entity, edx);
    ms_chase_deg += fabs(BamsDeltaDeg(before, *facing));
    ms_ticks++;
}

// The hand's muzzle: aim origin advanced along the ray (meters x
// world_scale), as in GateAndMuzzle.
inline void MuzzleForHand(int hand, float* out) {
    const float off =
        vrmod::config.gun_muzzle_offset_m * vrmod::config.world_scale;
    out[0] = aim_origin[hand][0] + aim_fwd[hand][0] * off;
    out[1] = aim_origin[hand][1] + aim_fwd[hand][1] * off;
    out[2] = aim_origin[hand][2] + aim_fwd[hand][2] * off;
}

// The damage multiplier slot for the current attack variant, or null
// for specials (variant 2) or when unreadable. The table is in read-only
// data, so writes go through VirtualProtect.
inline float* VariantDmgSlot(uintptr_t entity) {
    if (!diag::Accessible(entity + ENT_ATTACK_VARIANT_OFF, 2, false))
        return nullptr;
    const int v = *reinterpret_cast<const uint16_t*>(
        entity + ENT_ATTACK_VARIANT_OFF);
    if (v < 0 || v > 1)
        return nullptr;
    return reinterpret_cast<float*>(VARIANT_DMG_TABLE_ADDR +
                                    (uintptr_t)v * 4);
}

inline void WriteProtectedFloat(float* addr, float value) {
    DWORD old_protect = 0;
    if (VirtualProtect(addr, 4, PAGE_READWRITE, &old_protect)) {
        *addr = value;
        VirtualProtect(addr, 4, old_protect, &old_protect);
    }
}

// The split decision from the current state. Same enemy and part: the
// two halves stack to full damage. Different targets: each takes half.
inline void ComputeSplitState(uintptr_t entity) {
    hit_split = false;
    hit_same = false;
    hit_right_owns = false;
    hit_left_owns = false;
    hit_left_ok = false;
    hit_right_id = 0xFFFF;
    hit_right_part = 0;
    SyncLocks();
    hit_left_id = left_lock_id;
    hit_left_part = left_lock_part;
    bool right_targeted = false;
    if (diag::Accessible(entity + ENT_HIT_INDEX_OFF, 8, false)) {
        const int count =
            *reinterpret_cast<const int*>(entity + ENT_HIT_INDEX_OFF);
        if (count >= 1 && count <= 10) {
            hit_right_id = *reinterpret_cast<const uint16_t*>(
                entity + ENT_HIT_INDEX_OFF + 4);
            hit_right_part = *reinterpret_cast<const int16_t*>(
                entity + ENT_HIT_INDEX_OFF + 6);
            right_targeted = reinterpret_cast<PartLookupFn>(
                                 PART_LOOKUP_ADDR)(hit_right_id,
                                                   (int)hit_right_part) != 0;
        }
    }
    hit_left_ok =
        left_lock_valid &&
        reinterpret_cast<PartLookupFn>(PART_LOOKUP_ADDR)(
            hit_left_id, (int)hit_left_part) != 0;
    // Pair 0's owner: the right if its candidate set holds the enemy, the
    // left if it is the left lock's enemy (by id; the game picks the part).
    hit_right_owns = right_targeted && targetaim::RightSees(hit_right_id);
    hit_left_owns =
        right_targeted && hit_left_ok && hit_left_id == hit_right_id;
    // A split needs each hand on its own target: right = pair 0 (owned),
    // left = the left lock.
    if (hit_left_ok && hit_right_owns) {
        hit_same = hit_left_id == hit_right_id &&
                   hit_left_part == hit_right_part;
        hit_split = !hit_same;
    }
}


// Popup log lines while 'hitcensus' or 'mechdualtrace' is armed.
inline void PopupTrace(const char* what, uintptr_t victim, int number) {
    (void)what; (void)victim; (void)number;
}

inline void FlushPendingPopup() {
    if (!pending_popup)
        return;
    PopupTrace("flush", pending_victim, pending_number);
    pending_popup = false;
    if (original_popup != nullptr)
        original_popup(pending_pos, pending_colour, pending_kind,
                       pending_number);
}

// Popup hook: for our hits with dual mechguns, positive numbers only,
// hold the first number and add a second same-spot one into it, so both
// barrels on one target show the full total.
inline void* __cdecl PopupCtorHook(const float* pos, int colour, int kind,
                                   int number) {
    const uintptr_t victim = vrmod::apply_scope_victim;
    if (!vrmod::config.mechgun_merge_popups || dual_kind < 0 ||
        victim == 0 || number <= 0 || pos == nullptr) {
        if (victim != 0)
            PopupTrace(dual_kind < 0 ? "pass(no-dual)" : "pass", victim,
                       number);
        return original_popup(pos, colour, kind, number);
    }
    // Same enemy and spot (one body part); different parts stay separate.
    const float ddx = pos[0] - pending_pos[0];
    const float ddy = pos[1] - pending_pos[1];
    const float ddz = pos[2] - pending_pos[2];
    const bool same_spot = ddx * ddx + ddy * ddy + ddz * ddz <
                           POPUP_MERGE_RADIUS * POPUP_MERGE_RADIUS;
    if (pending_popup && !pending_merged && pending_victim == victim &&
        same_spot && popup_tick - pending_tick <= 1) {
        pending_number += number;
        pending_merged = true;
        PopupTrace("merge", victim, number);
        return nullptr;
    }
    FlushPendingPopup();
    PopupTrace("hold", victim, number);
    pending_popup = true;
    pending_merged = false;
    memcpy(pending_pos, pos, sizeof(pending_pos));
    pending_colour = colour;
    pending_kind = kind;
    pending_number = number;
    pending_victim = victim;
    pending_tick = popup_tick;
    return nullptr;
}

inline void OnFrame() {
    MaintainFacingSnap();
    // Flush the held popup once its partner's chance has passed.
    popup_tick++;
    if (pending_popup && popup_tick >= pending_tick + 2)
        FlushPendingPopup();
    aim_valid = false;
    aim_valid_l = false;
    dual_kind = -1;
    left_lock_valid = false;
    vrmod::mech_dual_active = false;
    vrmod::mech_hand_origin_valid[0] = false;
    vrmod::mech_hand_origin_valid[1] = false;
    if (!installed || !vrmod::config.gun_fire_origin ||
        !vrmod::config.weapon_grip)
        return;
    if (!gamecam::DrivesView())
        return;
    // The ray per hand: the barrel frame (weapongrip::BarrelRay).
    for (int h = 0; h < 2; h++) {
        if (!weapongrip::BarrelRay(h, aim_origin[h], aim_fwd[h],
                                   vrmod::config.gun_barrel_pitch_deg))
            continue;  // no pose (asleep/unfocused/OpenVR) -> vanilla
        if (h == 1)
            aim_valid = true;
        else
            aim_valid_l = true;
        // The hand as the targeting cone origin (psobbvr_targetaim.hpp).
        vrmod::mech_hand_origin_valid[h] = true;
        memcpy(vrmod::mech_hand_origin[h], aim_origin[h], sizeof(float) * 3);
    }
    // Once per launch: log the ray source.
    if (!logged_ray_source && aim_valid && weapongrip::current_is_gun) {
        logged_ray_source = true;
        const char* src = "BARREL frame (grip x gun tuple, -Y)";
        diag::Log("gunfire: free-fire ray = %s", src);
    }

    // Dual-mechgun detection: the held weapon's fire override (vtbl+0xCC)
    // is one of the mapped mechgun functions.
    if (mech_hooks_ok && vrmod::config.mechgun_dual && aim_valid &&
        aim_valid_l && weapongrip::current_is_gun) {
        void* w = weapongrip::current_weapon;
        const uintptr_t wa = reinterpret_cast<uintptr_t>(w);
        if (wa != 0 && diag::Accessible(wa, 4, false)) {
            const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(wa);
            if (vt != 0 && diag::Accessible(vt + 0xCC, 4, false)) {
                const uintptr_t ov =
                    *reinterpret_cast<const uintptr_t*>(vt + 0xCC);
                if (ov == MECH_FIRE_CAT8_ADDR) {
                    dual_kind = 0;
                    dual_slot_a = 0x240;
                    dual_slot_b = 0x24C;
                } else if (ov == MECH_FIRE_YAS_ADDR) {
                    dual_kind = 1;
                    dual_slot_a = 0x250;
                    dual_slot_b = 0x25C;
                } else if (ov == MECH_FIRE_BASE_ADDR) {
                    dual_kind = 2;
                    dual_slot_a = 0x240;
                    dual_slot_b = 0x24C;
                }
            }
        }
        if (dual_kind >= 0) {
            // For the targeting hooks: dual mode and the left ray's yaw
            // (the facing for the left copy pass).
            vrmod::mech_dual_active = true;
            const float t = atan2f(aim_fwd[0][0], aim_fwd[0][2]);
            vrmod::mech_left_yaw_bams =
                (uint32_t)lroundf(t * (65536.0f / 6.2831853f)) & 0xFFFF;
        }
    }
    SyncLocks();

}

// The untargeted-shot shim: passes the real callback our dest and ticks
// (from PrepareFreeFire). The bullet ctor (0x5E4158) takes an absolute
// dest. Shared by all callback variants.
inline void __cdecl ProjShim(void* weapon, float* source, float* dest,
                             float ticks) {
    const ProjSpawnFn cb = real_cb;
    if (cb == nullptr)
        return;
    if (!ff_active) {
        cb(weapon, source, dest, ticks);  // shouldn't happen; stay native
        return;
    }
    cb(weapon, source, ff_dest, ff_ticks);
}

// Shared gate: false = run native; true fills the muzzle and the owner
// entity.
inline bool GateAndMuzzle(void* weapon, float* muzzle, uintptr_t& entity) {
    if (!aim_valid || weapon == nullptr ||
        weapon != weapongrip::current_weapon || !weapongrip::current_is_gun)
        return false;
    const uintptr_t w = reinterpret_cast<uintptr_t>(weapon);
    if (!diag::Accessible(w + WEAPON_OWNER_OFF, 4, false))
        return false;
    entity = *reinterpret_cast<const uintptr_t*>(w + WEAPON_OWNER_OFF);
    if (entity == 0 ||
        !diag::Accessible(entity + ENT_HIT_INDEX_OFF, 0x2C, false))
        return false;

    // Right hand; twin mechguns go through the dual window instead.
    ff_hand = 1;

    // Aim origin advanced along the ray (meters x world_scale).
    const float off =
        vrmod::config.gun_muzzle_offset_m * vrmod::config.world_scale;
    muzzle[0] = aim_origin[ff_hand][0] + aim_fwd[ff_hand][0] * off;
    muzzle[1] = aim_origin[ff_hand][1] + aim_fwd[ff_hand][1] * off;
    muzzle[2] = aim_origin[ff_hand][2] + aim_fwd[ff_hand][2] * off;
    return true;
}

// Variant 0's own pair test at a 0-based index.
inline bool PairTargeted(uintptr_t entity, int index) {
    int count = *reinterpret_cast<const int*>(entity + ENT_HIT_INDEX_OFF);
    if (count < 0 || count > 10)
        count = 0;
    if (index < 0 || index >= count)
        return false;
    const uint16_t id = *reinterpret_cast<const uint16_t*>(
        entity + ENT_HIT_INDEX_OFF + 4 + index * 4);
    const int16_t part = *reinterpret_cast<const int16_t*>(
        entity + ENT_HIT_INDEX_OFF + 6 + index * 4);
    return reinterpret_cast<PartLookupFn>(PART_LOOKUP_ADDR)(id, (int)part) !=
           0;
}

// The gun buzz per fired bullet (gun_haptic 2), on the bullet's hand.
// Mode 1 (on trigger pull) is in psobbvr_controller.hpp.
inline void FireHaptic() {
    const auto& c = vrmod::config;
    if (c.gun_haptic == 2)
        vrmod::Get()->HapticPulse(ff_hand, c.gun_haptic_s, c.gun_haptic_amp);
}

inline void LogShot(bool targeted, const float* muzzle, int pair) {
    if (targeted && !logged_first_targeted) {
        logged_first_targeted = true;
        diag::Log("gunfire: first targeted shot redirected");
    }
    if (!targeted && !logged_first_free) {
        logged_first_free = true;
        diag::Log("gunfire: first free-fire shot redirected");
    }
}

// Variants 1/2 fire at the current pair: [entity+0x8D0] is the index
// into the id/part dwords at +0x8D0 (their own test, mirrored).
inline bool PrepareShot(void* weapon, float* muzzle, bool& targeted) {
    uintptr_t entity = 0;
    if (!GateAndMuzzle(weapon, muzzle, entity))
        return false;
    targeted = false;
    const int idx = *reinterpret_cast<const int*>(entity + ENT_HIT_INDEX_OFF);
    if (idx >= 1 && idx <= 10) {
        const uint16_t id = *reinterpret_cast<const uint16_t*>(
            entity + ENT_HIT_INDEX_OFF + idx * 4);
        const int16_t part = *reinterpret_cast<const int16_t*>(
            entity + ENT_HIT_INDEX_OFF + 2 + idx * 4);
        targeted =
            reinterpret_cast<PartLookupFn>(PART_LOOKUP_ADDR)(id, (int)part) !=
            0;
    }
    LogShot(targeted, muzzle, idx);
    FireHaptic();
    return true;
}

// Suppresses variant 0's own wall impact (along the character's line)
// during our free fire; PrepareFreeFire spawns ours.
inline int __cdecl WallImpactHook(void* mgr, void* weapon, float* source,
                                  float* hitpos, int ticks) {
    if (!ff_active)
        return original_impact(mgr, weapon, source, hitpos, ticks);
    return 0;
}

// Point the weapon's projectile callback at the shim for one call.
// Returns the saved callback, or null if the slot is empty/unreadable.
inline ProjSpawnFn ArmShim(void* weapon) {
    const uintptr_t w = reinterpret_cast<uintptr_t>(weapon);
    if (!diag::Accessible(w + WEAPON_PROJ_CB_OFF, 4, true))
        return nullptr;
    void** slot = reinterpret_cast<void**>(w + WEAPON_PROJ_CB_OFF);
    const ProjSpawnFn saved = reinterpret_cast<ProjSpawnFn>(*slot);
    if (saved == nullptr)
        return nullptr;
    real_cb = saved;
    *slot = reinterpret_cast<void*>(&ProjShim);
    return saved;
}

inline void DisarmShim(void* weapon, ProjSpawnFn saved) {
    void** slot = reinterpret_cast<void**>(
        reinterpret_cast<uintptr_t>(weapon) + WEAPON_PROJ_CB_OFF);
    *slot = reinterpret_cast<void*>(saved);
    real_cb = nullptr;
}

// One base-virtual call in the dual window; `hand` comes from which
// barrel slot the source is (already holding our muzzle). The left
// barrel fires at the left lock (pair 0 swapped for the call). Every
// targeted call deals damage (do_damage forced to 1, as the override
// passes 0 to its second call) at half the multiplier. A barrel on its
// own ray gets do_damage 0.
inline int DualShot(void* weapon, void* edx, float* source, int pair_index,
                    uint32_t param, int do_damage, int hand) {
    const uintptr_t w = reinterpret_cast<uintptr_t>(weapon);
    if (!diag::Accessible(w + WEAPON_OWNER_OFF, 4, false))
        return original0(weapon, edx, source, pair_index, param, do_damage);
    const uintptr_t entity =
        *reinterpret_cast<const uintptr_t*>(w + WEAPON_OWNER_OFF);
    if (entity == 0 ||
        !diag::Accessible(entity + ENT_HIT_INDEX_OFF, 0x2C, true))
        return original0(weapon, edx, source, pair_index, param, do_damage);

    ff_hand = hand;
    int dmg = do_damage;
    bool swapped = false;
    uint16_t* pair_id =
        reinterpret_cast<uint16_t*>(entity + ENT_HIT_INDEX_OFF + 4);
    int16_t* pair_part =
        reinterpret_cast<int16_t*>(entity + ENT_HIT_INDEX_OFF + 6);
    const uint16_t sv_id = *pair_id;
    const int16_t sv_part = *pair_part;
    // Per-barrel routing by ownership. want_pair=false = free fire along
    // the hand's own ray.
    bool want_pair = true;
    if (hand == 0) {
        if (!hit_left_ok) {
            // No left lock: shoot where the left hand points.
            want_pair = false;
        } else if (hit_split) {
            *pair_id = hit_left_id;
            *pair_part = hit_left_part;
            swapped = true;
        } else if (!hit_left_owns && !hit_same) {
            // Left lock on another enemy while the right does not own
            // pair 0 either: fly at the left lock.
            *pair_id = hit_left_id;
            *pair_part = hit_left_part;
            swapped = true;
        }
        // Otherwise pair 0: both on one target (hit_same), or the lock
        // is the left hand's alone ("mode L"; the right free-fires).
    } else {
        if (hit_left_owns && !hit_right_owns) {
            // Mode L: the lock is the left hand's and the right ray sees
            // nothing, so the right free-fires.
            want_pair = false;
        } else if (hit_split &&
                   (*pair_id != hit_right_id ||
                    *pair_part != hit_right_part)) {
            // Pin the right hand's pair (defensive).
            *pair_id = hit_right_id;
            *pair_part = hit_right_part;
            swapped = true;
        }
    }
    const bool targeted = want_pair && PairTargeted(entity, pair_index);
    // A free-firing barrel must not deal damage: the base virtual
    // schedules the impact on the passed pair whenever it is valid,
    // wherever the bullet flies, so in mode L the right barrel would add
    // a full hit to the left's half.
    if (!want_pair)
        dmg = 0;
    // Every targeted mechgun bullet deals half damage, so full damage
    // needs both barrels on one target. Specials (no multiplier slot)
    // stay vanilla.
    float* mult_slot = targeted ? VariantDmgSlot(entity) : nullptr;
    if (mult_slot != nullptr && dmg == 0)
        dmg = 1;
    LogShot(targeted, source, pair_index);
    FireHaptic();
    int ret;
    float saved_mult = 0.0f;
    if (mult_slot != nullptr) {
        saved_mult = *mult_slot;
        WriteProtectedFloat(mult_slot, saved_mult * 0.5f);
    }
    if (targeted) {
        ret = original0(weapon, edx, source, pair_index, param, dmg);
    } else {
        PrepareFreeFire(weapon, source, (int32_t)param);
        const ProjSpawnFn saved = ArmShim(weapon);
        ret = original0(weapon, edx, source, pair_index, param, dmg);
        if (saved != nullptr)
            DisarmShim(weapon, saved);
        ff_active = false;
    }
    if (mult_slot != nullptr)
        WriteProtectedFloat(mult_slot, saved_mult);
    if (swapped) {
        *pair_id = sv_id;
        *pair_part = sv_part;
    }
    return ret;
}

// The mechgun override wrapper: write our hand muzzles into the weapon's
// barrel slots for the call (so the base calls and the classes' own
// flare effects follow the controllers) and open the dual window for
// Hook0. Slot A (the damage-first call) is the right hand unless
// mechgun_swap_barrels.
inline void MechFireCommon(int kind, void* weapon, void* edx,
                           MechFireFn orig) {
    if (!logged_mech_call) {
        logged_mech_call = true;
        diag::Log("mechdual: first override call (kind %d weapon=%08X "
                  "ours=%d dual=%d)",
                  kind, (unsigned)(uintptr_t)weapon,
                  weapon == weapongrip::current_weapon ? 1 : 0, dual_kind);
    }
    const uintptr_t w = reinterpret_cast<uintptr_t>(weapon);
    if (dual_kind != kind || weapon == nullptr ||
        weapon != weapongrip::current_weapon ||
        !diag::Accessible(w + dual_slot_a, 12, true) ||
        !diag::Accessible(w + dual_slot_b, 12, true)) {
        orig(weapon, edx);
        return;
    }
    hit_split = false;
    hit_same = false;
    if (diag::Accessible(w + WEAPON_OWNER_OFF, 4, false)) {
        const uintptr_t entity =
            *reinterpret_cast<const uintptr_t*>(w + WEAPON_OWNER_OFF);
        if (entity != 0 &&
            diag::Accessible(entity + ENT_HIT_INDEX_OFF, 8, false))
            ComputeSplitState(entity);
    }
    float sva[3], svb[3], mza[3], mzb[3];
    float* slot_a = reinterpret_cast<float*>(w + dual_slot_a);
    float* slot_b = reinterpret_cast<float*>(w + dual_slot_b);
    memcpy(sva, slot_a, 12);
    memcpy(svb, slot_b, 12);
    const int hand_a = vrmod::config.mechgun_swap_barrels ? 0 : 1;
    MuzzleForHand(hand_a, mza);
    MuzzleForHand(1 - hand_a, mzb);
    memcpy(slot_a, mza, 12);
    memcpy(slot_b, mzb, 12);
    dual_window = true;
    dual_weapon = weapon;
    orig(weapon, edx);
    dual_window = false;
    dual_weapon = nullptr;
    memcpy(slot_a, sva, 12);
    memcpy(slot_b, svb, 12);
}

inline void __fastcall MechFireHook0(void* weapon, void* edx) {
    MechFireCommon(0, weapon, edx, original_mech0);
}
inline void __fastcall MechFireHook1(void* weapon, void* edx) {
    MechFireCommon(1, weapon, edx, original_mech1);
}
inline void __fastcall MechFireHook2(void* weapon, void* edx) {
    MechFireCommon(2, weapon, edx, original_mech2);
}

// Hit-report hook: on a split hit both victims go into subcommand
// 0x46's pair list (count 2, as a multi-target Shot reports), so the
// server sees both. The halved damage syncs through the appliers' own
// subcommands.
inline void __fastcall HitReportHook(void* entity, void* edx) {
    const uintptr_t e = reinterpret_cast<uintptr_t>(entity);
    if (dual_kind < 0 || e == 0 || e != gamecam::ResolveEntity() ||
        !diag::Accessible(e + ENT_HIT_INDEX_OFF, 0xC, true)) {
        original_report(entity, edx);
        return;
    }
    ComputeSplitState(e);
    int* count = reinterpret_cast<int*>(e + ENT_HIT_INDEX_OFF);
    uint16_t* pair1_id =
        reinterpret_cast<uint16_t*>(e + ENT_HIT_INDEX_OFF + 8);
    int16_t* pair1_part =
        reinterpret_cast<int16_t*>(e + ENT_HIT_INDEX_OFF + 10);
    const int sv_count = *count;
    const uint16_t sv_id = *pair1_id;
    const int16_t sv_part = *pair1_part;
    const bool expand = hit_split && sv_count == 1;
    if (expand) {
        *count = 2;
        *pair1_id = hit_left_id;
        *pair1_part = hit_left_part;
    }
    original_report(entity, edx);
    if (expand) {
        *count = sv_count;
        *pair1_id = sv_id;
        *pair1_part = sv_part;
    }
}

// The left marker's 180-degree flip (see RETICLE_ANGLE_ADDRS).
inline void WriteCodeDword(uintptr_t addr, uint32_t value) {
    DWORD old_protect = 0;
    void* p = reinterpret_cast<void*>(addr);
    if (VirtualProtect(p, 4, PAGE_EXECUTE_READWRITE, &old_protect)) {
        *reinterpret_cast<uint32_t*>(p) = value;
        VirtualProtect(p, 4, old_protect, &old_protect);
    }
}

inline void SetReticleFlip(bool flipped) {
    for (int i = 0; i < 3; i++)
        WriteCodeDword(RETICLE_ANGLE_ADDRS[i],
                       flipped
                           ? ((RETICLE_ANGLE_BASE[i] + 0x8000) & 0xFFFF)
                           : RETICLE_ANGLE_BASE[i]);
}

inline void RenderRotatedReticle(void* obj) {
    SetReticleFlip(true);
    reinterpret_cast<ReticleFn>(RETICLE_BIND_ADDR)(
        reinterpret_cast<void*>(RETICLE_BIND_OBJ), nullptr);
    reinterpret_cast<ReticleFn>(RETICLE_RENDER_ADDR)(obj, nullptr);
    SetReticleFlip(false);
}

// The rotated marker means "the left hand's target". The game keeps one
// reticle object, matching the current target (0x7A32EC kills others);
// +0x60 is its category (1 item / talk cursor, 2 attack, 3 technique).
// Only an attack reticle is copied (standing on an item makes the live
// object the item cursor).
// - Lock owned by the right hand (or shared): native draw plus the
//   rotated copy at the left lock.
// - Lock owned by the left hand only: the rotated marker instead, so
//   the normal look always means the right hand.
inline void __fastcall ReticleDrawHook(void* obj, void* edx) {
    const uintptr_t o = reinterpret_cast<uintptr_t>(obj);
    if (!vrmod::config.mechgun_reticle || dual_kind < 0 || o == 0 ||
        !diag::Accessible(o + 0x1C, 0x50, true) ||
        *reinterpret_cast<const uint8_t*>(o + 0x60) != 2) {
        original_reticle(obj, edx);
        return;
    }
    SyncLocks();
    const uint16_t tid = *reinterpret_cast<const uint16_t*>(o + 0x64);
    const bool is_left = left_lock_valid && tid == left_lock_id;
    const bool right_sees = targetaim::RightSees(tid);
    if (is_left && !right_sees) {
        // Mode L: the native draw's gates and position refresh, then the
        // flipped render only.
        const uint32_t flags =
            *reinterpret_cast<const uint32_t*>(o + 0x20);
        const int state = *reinterpret_cast<const int*>(o + 0x1C);
        if ((flags & 0xA) != 0 || state < 0 || state > 2)
            return;
        if (state <= 1)
            reinterpret_cast<ReticleFn>(RETICLE_REFRESH_ADDR)(obj,
                                                              nullptr);
        RenderRotatedReticle(obj);
        return;
    }
    original_reticle(obj, edx);
    if (!left_lock_valid)
        return;
    const uint32_t flags = *reinterpret_cast<const uint32_t*>(o + 0x20);
    const int state = *reinterpret_cast<const int*>(o + 0x1C);
    if ((flags & 0xA) != 0 || state < 0 || state > 2)
        return;  // the native draw skipped its render this frame
    void* ent = reinterpret_cast<EntityByIdFn>(ENTITY_BY_ID_ADDR)(
        (int)left_lock_id);
    const uintptr_t ea = reinterpret_cast<uintptr_t>(ent);
    if (ea == 0 || !diag::Accessible(ea + ENT_PARTS_OFF, 4, false))
        return;
    const uintptr_t parts =
        *reinterpret_cast<const uintptr_t*>(ea + ENT_PARTS_OFF);
    const uintptr_t rec =
        parts + (uintptr_t)(uint16_t)left_lock_part * 0x2C;
    if (parts == 0 || !diag::Accessible(rec + 0x1C, 12, false))
        return;
    const float* pos = reinterpret_cast<const float*>(rec + 0x1C);
    float* opos = reinterpret_cast<float*>(o + 0x40);
    const float sv[3] = {opos[0], opos[1], opos[2]};
    opos[0] = pos[0];
    opos[1] = pos[1];
    opos[2] = pos[2];
    RenderRotatedReticle(obj);
    opos[0] = sv[0];
    opos[1] = sv[1];
    opos[2] = sv[2];
}

// Variant 0: the base weapon virtual (vtbl+0xC8), firing at hit
// pair[pair_index] (0-based).
inline int __fastcall Hook0(void* weapon, void* edx, float* source,
                            int pair_index, uint32_t param, int do_damage) {
    if (!logged_v0_call) {
        logged_v0_call = true;
        diag::Log("gunfire: variant 0 first call (weapon=%08X ours=%d "
                  "gun=%d aim=%d src=%08X pair=%d)",
                  (unsigned)(uintptr_t)weapon,
                  weapon == weapongrip::current_weapon ? 1 : 0,
                  weapongrip::current_is_gun ? 1 : 0, aim_valid ? 1 : 0,
                  (unsigned)(uintptr_t)source, pair_index);
    }
    // In the dual window, the source pointer's barrel slot names the hand.
    if (dual_window && weapon == dual_weapon) {
        const uintptr_t w = reinterpret_cast<uintptr_t>(weapon);
        const int hand_a = vrmod::config.mechgun_swap_barrels ? 0 : 1;
        int hand = -1;
        if (reinterpret_cast<uintptr_t>(source) == w + dual_slot_a)
            hand = hand_a;
        else if (reinterpret_cast<uintptr_t>(source) == w + dual_slot_b)
            hand = 1 - hand_a;
        if (hand >= 0)
            return DualShot(weapon, edx, source, pair_index, param,
                            do_damage, hand);
    }
    float muzzle[3];
    uintptr_t entity = 0;
    if (!GateAndMuzzle(weapon, muzzle, entity))
        return original0(weapon, edx, source, pair_index, param, do_damage);
    const bool targeted = PairTargeted(entity, pair_index);
    LogShot(targeted, muzzle, pair_index);
    FireHaptic();
    if (targeted)
        return original0(weapon, edx, muzzle, pair_index, param, do_damage);
    PrepareFreeFire(weapon, muzzle, (int32_t)param);
    const ProjSpawnFn saved = ArmShim(weapon);
    const int ret =
        original0(weapon, edx, muzzle, pair_index, param, do_damage);
    if (saved != nullptr)
        DisarmShim(weapon, saved);
    ff_active = false;
    return ret;
}

inline void __fastcall Hook(void* weapon, void* edx, float mult,
                            float* source, int do_damage) {
    if (!logged_v1_call) {
        logged_v1_call = true;
        diag::Log("gunfire: variant 1 first call (weapon=%08X ours=%d "
                  "gun=%d aim=%d src=%08X)",
                  (unsigned)(uintptr_t)weapon,
                  weapon == weapongrip::current_weapon ? 1 : 0,
                  weapongrip::current_is_gun ? 1 : 0, aim_valid ? 1 : 0,
                  (unsigned)(uintptr_t)source);
    }
    float muzzle[3];
    bool targeted = false;
    if (!PrepareShot(weapon, muzzle, targeted)) {
        original(weapon, edx, mult, source, do_damage);
        return;
    }
    if (targeted) {
        original(weapon, edx, mult, muzzle, do_damage);
        return;
    }
    PrepareFreeFire(weapon, muzzle);
    const ProjSpawnFn saved = ArmShim(weapon);
    original(weapon, edx, mult, muzzle, do_damage);
    if (saved != nullptr)
        DisarmShim(weapon, saved);
    ff_active = false;
}

inline void __fastcall Hook2(void* weapon, void* edx, float* source,
                             uint32_t param, int yaw_offset) {
    if (!logged_v2_call) {
        logged_v2_call = true;
        diag::Log("gunfire: variant 2 first call (weapon=%08X ours=%d "
                  "gun=%d aim=%d src=%08X)",
                  (unsigned)(uintptr_t)weapon,
                  weapon == weapongrip::current_weapon ? 1 : 0,
                  weapongrip::current_is_gun ? 1 : 0, aim_valid ? 1 : 0,
                  (unsigned)(uintptr_t)source);
    }
    float muzzle[3];
    bool targeted = false;
    if (!PrepareShot(weapon, muzzle, targeted)) {
        original2(weapon, edx, source, param, yaw_offset);
        return;
    }
    if (targeted) {
        original2(weapon, edx, muzzle, param, yaw_offset);
        return;
    }
    PrepareFreeFire(weapon, muzzle);
    const ProjSpawnFn saved = ArmShim(weapon);
    original2(weapon, edx, muzzle, param, yaw_offset);
    if (saved != nullptr)
        DisarmShim(weapon, saved);
    ff_active = false;
}

// Variant 3: the launcher family, locked on hit pair 0 (count
// [owner+0x8D0] >= 1, id/part at +0x8D4/6). Unlocked, the shell flies
// along [owner+0x60] + yaw_offset, so free fire passes the offset that
// turns the facing into the aim yaw, and ShellSpawnHook adds the aim
// pitch. No shim or PrepareFreeFire: the shell has its own explosion.
inline void __fastcall Hook3(void* weapon, void* edx, float* source,
                             uint32_t param, int yaw_offset) {
    if (!logged_v3_call) {
        logged_v3_call = true;
        diag::Log("gunfire: variant 3 (launcher) first call (weapon=%08X "
                  "ours=%d gun=%d aim=%d src=%08X param=%u yawoff=%d)",
                  (unsigned)(uintptr_t)weapon,
                  weapon == weapongrip::current_weapon ? 1 : 0,
                  weapongrip::current_is_gun ? 1 : 0, aim_valid ? 1 : 0,
                  (unsigned)(uintptr_t)source, param, yaw_offset);
    }
    float muzzle[3];
    uintptr_t entity = 0;
    if (!GateAndMuzzle(weapon, muzzle, entity)) {
        original3(weapon, edx, source, param, yaw_offset);
        return;
    }
    const bool targeted = PairTargeted(entity, 0);
    LogShot(targeted, muzzle, 0);
    FireHaptic();
    if (targeted) {
        original3(weapon, edx, muzzle, param, yaw_offset);
        return;
    }
    const int32_t facing =
        *reinterpret_cast<const int32_t*>(entity + ENT_FACING_OFF);
    const float aim_yaw = atan2f(aim_fwd[ff_hand][0], aim_fwd[ff_hand][2]);
    const int32_t aim_bams =
        (int32_t)(aim_yaw * (65536.0f / 6.283185307f) + (aim_yaw >= 0 ? 0.5f : -0.5f));
    const int32_t off = (int32_t)(int16_t)((aim_bams - facing) & 0xFFFF);
    v3_window = true;
    original3(weapon, edx, muzzle, param, off);
    v3_window = false;
}

// In our variant-3 window the shell gets the full aim direction, pitch
// included; every other call passes through.
inline void* __cdecl ShellSpawnHook(void* mgr, float* desc) {
    if (v3_window && desc != nullptr) {
        float* dir = reinterpret_cast<float*>(
            reinterpret_cast<uintptr_t>(desc) + DESC_DIR_OFF);
        uint32_t* flags = reinterpret_cast<uint32_t*>(
            reinterpret_cast<uintptr_t>(desc) + DESC_FLAGS_OFF);
        dir[0] = aim_fwd[ff_hand][0];
        dir[1] = aim_fwd[ff_hand][1];
        dir[2] = aim_fwd[ff_hand][2];
        *flags |= 1u;
        if (!logged_v3_spawn) {
            logged_v3_spawn = true;
            diag::Log("gunfire: launcher shell descriptor redirected (dir %.2f %.2f %.2f, flags %X)",
                      dir[0], dir[1], dir[2], *flags);
        }
    }
    return original_spawn(mgr, desc);
}

inline bool InstallOne(uintptr_t addr, void* hook, void** orig,
                       const char* name) {
    if (MH_CreateHook(reinterpret_cast<void*>(addr), hook, orig) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(addr)) != MH_OK) {
        probe::Log("gunfire: %s hook failed", name);
        diag::Log("gunfire: %s hook failed", name);
        return false;
    }
    return true;
}

// Called from the first BeginScene; behavior is gated per frame and shot.
inline void Install() {
    static bool tried = false;
    if (tried)
        return;
    tried = true;

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        probe::Log("gunfire: MH_Initialize failed (%d)", (int)status);
        diag::Log("gunfire: MH_Initialize failed (%d)", (int)status);
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(SHOT_FIRE0_ADDR),
                      reinterpret_cast<void*>(&Hook0),
                      reinterpret_cast<void**>(&original0)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(SHOT_FIRE0_ADDR)) != MH_OK) {
        probe::Log("gunfire: variant-0 hook failed");
        diag::Log("gunfire: variant-0 hook failed");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(SHOT_FIRE_ADDR),
                      reinterpret_cast<void*>(&Hook),
                      reinterpret_cast<void**>(&original)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(SHOT_FIRE_ADDR)) != MH_OK) {
        probe::Log("gunfire: variant-1 hook failed");
        diag::Log("gunfire: variant-1 hook failed");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(SHOT_FIRE2_ADDR),
                      reinterpret_cast<void*>(&Hook2),
                      reinterpret_cast<void**>(&original2)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(SHOT_FIRE2_ADDR)) != MH_OK) {
        probe::Log("gunfire: variant-2 hook failed");
        diag::Log("gunfire: variant-2 hook failed");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(SHOT_FIRE3_ADDR),
                      reinterpret_cast<void*>(&Hook3),
                      reinterpret_cast<void**>(&original3)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(SHOT_FIRE3_ADDR)) != MH_OK) {
        probe::Log("gunfire: variant-3 hook failed");
        diag::Log("gunfire: variant-3 hook failed");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(SHELL_SPAWN_ADDR),
                      reinterpret_cast<void*>(&ShellSpawnHook),
                      reinterpret_cast<void**>(&original_spawn)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(SHELL_SPAWN_ADDR)) != MH_OK) {
        probe::Log("gunfire: shell-spawn hook failed");
        diag::Log("gunfire: shell-spawn hook failed");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(WALL_IMPACT_ADDR),
                      reinterpret_cast<void*>(&WallImpactHook),
                      reinterpret_cast<void**>(&original_impact)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(WALL_IMPACT_ADDR)) != MH_OK) {
        probe::Log("gunfire: wall-impact hook failed");
        diag::Log("gunfire: wall-impact hook failed");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(STAGE_MACHINE_ADDR),
                      reinterpret_cast<void*>(&StageMachineHook),
                      reinterpret_cast<void**>(&original_stage)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(STAGE_MACHINE_ADDR)) != MH_OK) {
        probe::Log("gunfire: stage-machine hook failed");
        diag::Log("gunfire: stage-machine hook failed");
        return;
    }
    installed = true;
    probe::Log("gunfire: shot-fire hooks installed (0x%08X, 0x%08X, 0x%08X, launcher 0x%08X + spawner 0x%08X)",
               (unsigned)SHOT_FIRE0_ADDR, (unsigned)SHOT_FIRE_ADDR,
               (unsigned)SHOT_FIRE2_ADDR, (unsigned)SHOT_FIRE3_ADDR, (unsigned)SHELL_SPAWN_ADDR);
    diag::Log("gunfire: shot-fire hooks installed (0x%08X, 0x%08X, 0x%08X, launcher 0x%08X + spawner 0x%08X)",
              (unsigned)SHOT_FIRE0_ADDR, (unsigned)SHOT_FIRE_ADDR,
              (unsigned)SHOT_FIRE2_ADDR, (unsigned)SHOT_FIRE3_ADDR, (unsigned)SHELL_SPAWN_ADDR);

    // Optional: a failure here disables only the dual-mechgun mode.
    mech_hooks_ok =
        InstallOne(MECH_FIRE_CAT8_ADDR,
                   reinterpret_cast<void*>(&MechFireHook0),
                   reinterpret_cast<void**>(&original_mech0),
                   "mechdual cat8") &&
        InstallOne(MECH_FIRE_YAS_ADDR,
                   reinterpret_cast<void*>(&MechFireHook1),
                   reinterpret_cast<void**>(&original_mech1),
                   "mechdual yasminkov") &&
        InstallOne(MECH_FIRE_BASE_ADDR,
                   reinterpret_cast<void*>(&MechFireHook2),
                   reinterpret_cast<void**>(&original_mech2),
                   "mechdual base") &&
        InstallOne(HIT_REPORT_ADDR,
                   reinterpret_cast<void*>(&HitReportHook),
                   reinterpret_cast<void**>(&original_report),
                   "mechdual hit-report");
    reticle_hook_ok =
        InstallOne(RETICLE_DRAW_ADDR,
                   reinterpret_cast<void*>(&ReticleDrawHook),
                   reinterpret_cast<void**>(&original_reticle),
                   "mechdual reticle");
    popup_hook_ok =
        InstallOne(POPUP_CTOR_ADDR,
                   reinterpret_cast<void*>(&PopupCtorHook),
                   reinterpret_cast<void**>(&original_popup),
                   "mechdual popup");
    diag::Log("mechdual: dual-fire hooks %s, reticle hook %s, popup hook %s",
              mech_hooks_ok ? "installed" : "FAILED",
              reticle_hook_ok ? "installed" : "FAILED",
              popup_hook_ok ? "installed" : "FAILED");
}

}  // namespace gunfire
