#pragma once

// Steers the game's target selection with the controller or the head,
// and gives the left mechgun its own lock through the same machinery.
//
// Each tick, targetable entities register with each player's 14
// candidate banks through the test+insert function
//   0x007A2B5C  __thiscall (bank /*ecx*/, candidate_entity,
//                           owner_player_entity, best_scratch), ret 0xC
// whose only aim direction is the owner's facing [entity+0x60] (BAMS,
// 0 = +z, forward = (sin, 0, cos)). The hook swaps that field to our aim
// yaw for the call and restores it, so every cone test follows and the
// request / commit logic is untouched.
//
// Bank layout (0x124 bytes each, 14 at targeting+0x84; the targeting
// object is [entity+0x45C]): byte +0x00 category = palette action type
// (1 context, 2 attack, 3 technique, 4 photon blast, 5 item), +0x01 its
// param, word +0x02 flags (bit 0 = stale until the bank writer 0x7A19C0
// configures it), dword +0x04 max locks, +0x08/+0x0C part-flag masks,
// +0x10/+0x14 entity-flag masks (required / forbidden), +0x18 largest
// inserted distance^2, vec3 +0x1C aim origin, +0x28 range, +0x2C/+0x30
// yaw/vertical cone cosines, +0x34/+0x38 their half-angles (BAMS), +0x3C
// push-back behind the origin, dword +0x40 candidate count, +0x44
// records (stride 8: word id, word part, float distance^2; cap 10, worst
// replaced), +0x9C/+0xA0 the output list. The config driver 0x7A1728
// orders banks by sort key (1 context, 2 attack, techs >= 3 from byte
// table 0x9FF674, 0xF photon blast), so attack banks commit first and a
// tech bank commits only when every attack bank is empty.
//
// Aim sources: with a gun, the right hand's barrel ray; melee/unarmed,
// the head's forward. The basic melee bank is omnidirectional (cos -1,
// range 15), so aim only matters where a consumer applies its own cone.
//
// Tech banks (category 3, param = tech id) follow the left hand's aim
// ray. Their geometry comes from the tech record (0x9CF3C0 + id*0x2C:
// +0x18 yaw full angle BAMS, +0x1C vertical, +0x22 max locks), halved by
// 0x6DED14 and stored by 0x7A19C0. A full angle of 0x10000 (cos -1) or 0
// (cos 1, the test skips the cone) is omnidirectional: those techs pick
// the nearest target and run stock. Directional techs get the facing
// swap; single-target ones also take the tighter of their cone and
// cast_aim_cone_deg. cast_aim_techs (-1 = from the record) is a tech-id
// bitmask override.
//
// Dual mechguns (vrmod::mech_dual_active): the game has one target slot,
// so its own test decides both hands.
//  - After each right-hand test, the hook reruns the test on a private
//    copy of the first attack bank with the left yaw, the same cone and
//    the left hand's x/z as origin. The test's only outside writes are
//    part marks (part flags bit 28) that the commit re-establishes, so
//    the copy leaves no trace. Its records are the left candidates.
//  - The finalizer 0x7A2290 (sort + commit per bank) is hooked. If the
//    right set is empty, the left candidates are written into every
//    attack bank so the game commits the left hand's enemy natively. If
//    the request pair names a left candidate the right pass lacks, that
//    record is appended so the request keeps committing. After the
//    finalizer the commit (+0x108C id, +0x1088 category, +0x108E part)
//    is owned by whichever hand's set contains it.
//
// Gun targeting measures from the aiming hand: the attack banks' origin
// x/z is the hand, since a held gun sits to the side of the body.
//
// Config [vr]: target_aim bits (1 guns, 2 melee gaze, 4 tech banks on the
// left ray), target_aim_cone_scale (gun yaw cone scale, restrictive cones
// only), mechgun_union (the left hand-off).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "MinHook.h"
#include "psobbvr_gamecam.hpp"
#include "psobbvr_log.hpp"
#include "psobbvr_probe.hpp"
#include "psobbvr_vr.hpp"
#include "psobbvr_weapongrip.hpp"

namespace targetaim {

constexpr uintptr_t CANDIDATE_TEST_ADDR = 0x007A2B5C;  // see header comment
// Per-player targeting finalizer (driver vtable 0xB45F68): sorts each
// live bank by distance^2, commits banks 0..13 in order (first wins),
// clears them. thiscall on the driver; [driver+0x20] = player entity.
constexpr uintptr_t FINALIZER_ADDR = 0x007A2290;
constexpr uintptr_t DRIVER_ENTITY_OFF = 0x20;
constexpr uintptr_t ENT_TARGETING_OFF = 0x45C;
constexpr uintptr_t ENT_FACING_OFF = 0x60;
// Targeting object fields.
constexpr uintptr_t TGT_BANKS_OFF = 0x84;
constexpr uintptr_t TGT_BANK_STRIDE = 0x124;
constexpr int TGT_BANK_COUNT = 14;
constexpr uintptr_t TGT_CUR_CAT_OFF = 0x1088;   // byte: committing bank's category
constexpr uintptr_t TGT_CUR_ID_OFF = 0x108C;    // word: current target id, 0xFFFF none
constexpr uintptr_t TGT_CUR_PART_OFF = 0x108E;  // word: current target part
constexpr uintptr_t TGT_REQ_ID_OFF = 0x1098;    // word: requested id (the lock)
constexpr uintptr_t TGT_REQ_PART_OFF = 0x109A;  // word: requested part
// Bank fields (relative to the bank).
constexpr uintptr_t BANK_CAT_OFF = 0x00;
constexpr uintptr_t BANK_FLAGS_OFF = 0x02;      // word; bit 0 = stale
constexpr uintptr_t BANK_MAXLOCK_OFF = 0x04;
constexpr uintptr_t BANK_MAXD2_OFF = 0x18;
constexpr uintptr_t BANK_ORIGIN_OFF = 0x1C;     // vec3
constexpr uintptr_t BANK_RANGE_OFF = 0x28;
constexpr uintptr_t BANK_YAW_COS_OFF = 0x2C;
constexpr uintptr_t BANK_VERT_COS_OFF = 0x30;
constexpr uintptr_t BANK_YAW_HALF_OFF = 0x34;   // BAMS
constexpr uintptr_t BANK_VERT_HALF_OFF = 0x38;  // BAMS
constexpr uintptr_t BANK_PUSH_OFF = 0x3C;
constexpr uintptr_t BANK_COUNT_OFF = 0x40;
constexpr uintptr_t BANK_RECORDS_OFF = 0x44;    // stride 8, cap 10
constexpr int BANK_RECORD_CAP = 10;
constexpr uint8_t BANK_CAT_ATTACK = 2;
constexpr uint8_t BANK_CAT_TECH = 3;

// __fastcall stands in for the game's thiscall (edx unused); the three
// stack args are callee-cleaned (ret 0xC), which __fastcall reproduces.
using TestFn = int(__fastcall*)(void* bank, void* edx, uintptr_t candidate,
                                uintptr_t owner, int* scratch);
using FinalizerFn = void(__fastcall*)(void* driver, void* edx);
inline TestFn original = nullptr;
inline FinalizerFn original_fin = nullptr;
inline bool installed = false;
inline bool fin_installed = false;

// Aim state computed once per frame in OnFrame, so the hook (run for
// every candidate test) stays cheap.
inline bool aim_valid = false;
inline uint32_t aim_yaw_bams = 0;
inline uintptr_t cached_entity = 0;
inline char aim_source = '-';  // 'G' gun ray / 'M' gaze / 'F' forced
// The left hand's aim ray, used only by tech (category 3) banks.
inline bool tech_aim_valid = false;
inline uint32_t tech_yaw_bams = 0;

constexpr float BAMS_PER_RADIAN = 65536.0f / 6.2831853f;
constexpr float DEG_TO_RAD = 3.14159265f / 180.0f;

inline uint32_t YawFromDirection(float x, float z) {
    // Facing convention: forward = (sin t, 0, cos t).
    const float t = atan2f(x, z);
    const long v = lroundf(t * BAMS_PER_RADIAN);
    return (uint32_t)v & 0xFFFF;
}

// ---- dual-mechgun lock state -------------------------------------------

struct Cand {
    uint16_t id;
    int16_t part;
    float d2;
};

// One targeting tick's result for the local player, written by the
// finalizer hook and read by gunfire on the next tick (the same latency
// as the game's attack pairs).
struct DualState {
    bool valid = false;        // produced by the finalizer this tick
    bool active = false;       // dual mode was on for the tick
    // The attack bank's records under the right ray, before any hand-off.
    int right_count = 0;
    Cand right[BANK_RECORD_CAP] = {};
    // The LEFT ray's candidates (the copy pass), nearest first.
    int left_count = 0;
    Cand left[BANK_RECORD_CAP] = {};
    bool left_valid = false;   // left[0] is the left hand's lock
    // The hand-off: 1 = the left list fed an empty attack bank, 2 = one
    // requested left record was appended beside the right set.
    int union_fed = 0;
    // The game's commit after the finalizer.
    bool cur_valid = false;
    uint16_t cur_id = 0xFFFF;
    int16_t cur_part = 0;
    uint8_t cur_cat = 0;
    bool right_owns = false;   // commit from an attack bank, in the right set
    bool left_owns = false;    // commit from an attack bank, a left candidate
};
inline DualState dual = {};

// The first live attack bank's candidates this tick, nearest first,
// taken before the commit. The basic melee bank is omnidirectional, so
// for melee this is every enemy in reach; gunfire's re-lock applies its
// own cone (retarget_cone_deg).
inline Cand aim_cands[BANK_RECORD_CAP] = {};
inline int aim_cands_n = 0;
inline char aim_cands_source = '-';

// Left candidates gathered by the copy pass; consumed by the finalizer.
inline Cand acc_left[BANK_RECORD_CAP] = {};
inline int acc_left_n = 0;
// The last copy pass result, for 'targetaimtrace'.
inline int last_left_ins = 0;
inline float last_left_d2 = 0.0f;

inline bool InList(const Cand* list, int n, uint16_t id) {
    for (int i = 0; i < n; i++)
        if (list[i].id == id)
            return true;
    return false;
}
inline bool InListPair(const Cand* list, int n, uint16_t id, int16_t part) {
    for (int i = 0; i < n; i++)
        if (list[i].id == id && list[i].part == part)
            return true;
    return false;
}
// Sorted insert (ascending distance^2), capped: the worst drops.
inline void AccInsert(Cand* list, int& n, uint16_t id, int16_t part,
                      float d2) {
    if (InListPair(list, n, id, part))
        return;
    int pos = n;
    while (pos > 0 && list[pos - 1].d2 > d2)
        pos--;
    if (pos >= BANK_RECORD_CAP)
        return;
    const int last = n < BANK_RECORD_CAP ? n : BANK_RECORD_CAP - 1;
    for (int i = last; i > pos; i--)
        list[i] = list[i - 1];
    list[pos] = {id, part, d2};
    if (n < BANK_RECORD_CAP)
        n++;
}

// Reader for gunfire.
inline bool RightSees(uint16_t id) {
    return dual.valid && InList(dual.right, dual.right_count, id);
}

// Called every frame from BeginScene, after gamecam::Apply and
// weapongrip::OnFrame (current_is_gun fresh).
inline void OnFrame() {
    aim_valid = false;
    tech_aim_valid = false;
    aim_source = '-';
    if (!installed || vrmod::config.target_aim == 0)
        return;
    if (!gamecam::DrivesView())
        return;
    cached_entity = gamecam::ResolveEntity();
    if (cached_entity == 0)
        return;

    // Diagnostic: a forced world yaw overrides the pose sources (tech
    // banks too).
    if (vrmod::config.target_aim_force_deg > -400.0f) {
        aim_yaw_bams = (uint32_t)lroundf(vrmod::config.target_aim_force_deg *
                                         (65536.0f / 360.0f)) & 0xFFFF;
        aim_source = 'F';
        aim_valid = true;
        if (vrmod::config.target_aim & 4) {
            tech_yaw_bams = aim_yaw_bams;
            tech_aim_valid = true;
        }
        return;
    }

    if (weapongrip::current_is_gun) {
        if (vrmod::config.target_aim & 1) {
            // The same ray the bullets use: the barrel frame
            // (weapongrip::BarrelRay). No pose (asleep/unfocused/OpenVR)
            // -> stock facing.
            float org[3], fwd[3];
            if (weapongrip::BarrelRay(1, org, fwd,
                                      vrmod::config.gun_barrel_pitch_deg)) {
                aim_yaw_bams = YawFromDirection(fwd[0], fwd[2]);
                aim_source = 'G';
                aim_valid = true;
            }
        }
    } else if (vrmod::config.target_aim & 2) {
        // Melee/unarmed: the head pose's forward (-z row).
        if (gamecam::written_this_frame) {
            aim_yaw_bams = YawFromDirection(-gamecam::world_from_head._31,
                                            -gamecam::world_from_head._33);
            aim_source = 'M';
            aim_valid = true;
        }
    }

    // The left hand's aim ray for tech banks.
    if (vrmod::config.target_aim & 4) {
        D3DMATRIX pose, world;
        if (vrmod::Get()->GetHandAimPose(0, pose) &&
            gamecam::WorldFromTracking(pose, world)) {
            tech_yaw_bams = YawFromDirection(-world._31, -world._33);
            tech_aim_valid = true;
        }
    }
}

// The bank's index within its targeting object, or -1.
inline int BankIndex(uintptr_t bank, uintptr_t tgt) {
    if (tgt == 0 || bank < tgt + TGT_BANKS_OFF)
        return -1;
    const uintptr_t off = bank - (tgt + TGT_BANKS_OFF);
    if (off % TGT_BANK_STRIDE != 0)
        return -1;
    const uintptr_t idx = off / TGT_BANK_STRIDE;
    return idx < (uintptr_t)TGT_BANK_COUNT ? (int)idx : -1;
}
inline uintptr_t BankAt(uintptr_t tgt, int idx) {
    return tgt + TGT_BANKS_OFF + (uintptr_t)idx * TGT_BANK_STRIDE;
}
inline bool BankLive(uintptr_t bank, uint8_t cat) {
    const uint8_t* b = reinterpret_cast<const uint8_t*>(bank);
    return b[BANK_CAT_OFF] == cat &&
           (*reinterpret_cast<const uint16_t*>(b + BANK_FLAGS_OFF) & 1) == 0;
}
// First live attack bank of the targeting object, or -1.
inline int FirstAttackBank(uintptr_t tgt) {
    for (int i = 0; i < TGT_BANK_COUNT; i++)
        if (BankLive(BankAt(tgt, i), BANK_CAT_ATTACK))
            return i;
    return -1;
}

// The scratch block the test updates (targeting +0x10A0, initialized by
// 0x7A1548).
struct Scratch {
    int32_t part;
    uint16_t pad;
    uint16_t id;
    int32_t zero;
    float best_d2;
};

// Gun yaw cone: the bank's half-angle (+0x34) times target_aim_cone_scale,
// written over the cosine the test uses (+0x2C). Half-angles of 0 or over
// 90 degrees (omni/utility banks) are left alone. Used by both hands.
inline void ApplyGunConeScale(uint8_t* bank) {
    const float scale = vrmod::config.target_aim_cone_scale;
    if (scale >= 0.999f && scale <= 1.001f)
        return;
    const uint32_t half =
        *reinterpret_cast<const uint32_t*>(bank + BANK_YAW_HALF_OFF) & 0xFFFF;
    if (half == 0 || half > 0x4000)
        return;
    *reinterpret_cast<float*>(bank + BANK_YAW_COS_OFF) =
        cosf((float)half * scale * (6.2831853f / 65536.0f));
}

// The left hand's copy pass for one candidate (see header).
inline void LeftCopyTest(uintptr_t bank, void* edx, uintptr_t candidate,
                         uintptr_t owner) {
    alignas(4) uint8_t copy[TGT_BANK_STRIDE];
    memcpy(copy, reinterpret_cast<const void*>(bank), sizeof(copy));
    *reinterpret_cast<int32_t*>(copy + BANK_COUNT_OFF) = 0;
    *reinterpret_cast<float*>(copy + BANK_MAXD2_OFF) = 0.0f;
    // The copy holds the stock cosine (the right pass restored it), so
    // apply the same scale.
    ApplyGunConeScale(copy);
    if (vrmod::mech_hand_origin_valid[0]) {
        *reinterpret_cast<float*>(copy + BANK_ORIGIN_OFF) =
            vrmod::mech_hand_origin[0][0];
        *reinterpret_cast<float*>(copy + BANK_ORIGIN_OFF + 8) =
            vrmod::mech_hand_origin[0][2];
    }
    Scratch sc = {0, 0xFFFF, 0xFFFF, 0, 640000.0f};
    int32_t* facing = reinterpret_cast<int32_t*>(owner + ENT_FACING_OFF);
    const int32_t saved = *facing;
    *facing = (int32_t)vrmod::mech_left_yaw_bams;
    const int ins = original(copy, edx, candidate, owner,
                             reinterpret_cast<int*>(&sc));
    *facing = saved;
    last_left_ins = ins;
    last_left_d2 = -1.0f;
    if (ins == 0)
        return;
    const int n = *reinterpret_cast<const int32_t*>(copy + BANK_COUNT_OFF);
    for (int i = 0; i < n && i < BANK_RECORD_CAP; i++) {
        const uint8_t* rec = copy + BANK_RECORDS_OFF + (uintptr_t)i * 8;
        const uint16_t id = *reinterpret_cast<const uint16_t*>(rec);
        const int16_t part = *reinterpret_cast<const int16_t*>(rec + 2);
        const float d2 = *reinterpret_cast<const float*>(rec + 4);
        if (id == 0xFFFF || !(d2 == d2))
            continue;
        if (last_left_d2 < 0.0f || d2 < last_left_d2)
            last_left_d2 = d2;
        AccInsert(acc_left, acc_left_n, id, part, d2);
    }
}

inline int __fastcall Hook(void* bank, void* edx, uintptr_t candidate,
                           uintptr_t owner, int* scratch) {
    // Only the local player's banks while an aim exists; others run stock.
    // ResolveEntity already checked cached_entity is readable.
    if ((!aim_valid && !tech_aim_valid) || owner == 0 ||
        owner != cached_entity)
        return original(bank, edx, candidate, owner, scratch);

    // Tech banks use the left hand's ray, but only for techs with a real
    // yaw cone that pass the cast_aim_techs mask; the rest run stock.
    const uint8_t* bank_hdr = reinterpret_cast<const uint8_t*>(bank);
    const uintptr_t bank_addr = reinterpret_cast<uintptr_t>(bank);
    bool tech_single = false;
    if (bank_hdr[0] == BANK_CAT_TECH) {
        const unsigned tech_id = bank_hdr[1];
        // Yaw cosine <= -1 passes everything, >= 1 skips the test: both
        // omnidirectional.
        const float stock_cos =
            *reinterpret_cast<const float*>(bank_addr + BANK_YAW_COS_OFF);
        const bool omni = stock_cos <= -1.0f || stock_cos >= 1.0f;
        const int mask = vrmod::config.cast_aim_techs;
        const bool listed =
            mask < 0 || (tech_id < 32 && ((mask >> tech_id) & 1) != 0);
        if (omni || !listed)
            return original(bank, edx, candidate, owner, scratch);
        tech_single =
            *reinterpret_cast<const int32_t*>(bank_addr + BANK_MAXLOCK_OFF) == 1;
    }
    const bool tech_bank = bank_hdr[0] == BANK_CAT_TECH && tech_aim_valid;
    if (!tech_bank && !aim_valid)
        return original(bank, edx, candidate, owner, scratch);

    int32_t* facing = reinterpret_cast<int32_t*>(owner + ENT_FACING_OFF);
    const int32_t saved = *facing;
    *facing = (int32_t)(tech_bank ? tech_yaw_bams : aim_yaw_bams);

    // Cone and origin changes below are restored after the call, like the
    // facing. The vertical cone stays stock so targets on slopes are kept.
    float* yaw_cos = reinterpret_cast<float*>(bank_addr + BANK_YAW_COS_OFF);
    float* vert_cos = reinterpret_cast<float*>(bank_addr + BANK_VERT_COS_OFF);
    float* origin = reinterpret_cast<float*>(bank_addr + BANK_ORIGIN_OFF);
    const float saved_cos = *yaw_cos;
    const float saved_vert = *vert_cos;
    const float saved_ox = origin[0];
    const float saved_oz = origin[2];
    const bool gun_source = !tech_bank && aim_source == 'G';
    if (tech_bank) {
        // Multi-target techs keep their own cone; single-target ones take
        // the tighter of it and cast_aim_cone_deg.
        if (tech_single) {
            const float c = cosf(vrmod::config.cast_aim_cone_deg * DEG_TO_RAD);
            if (c > *yaw_cos)
                *yaw_cos = c;
        }
    } else if (gun_source) {
        ApplyGunConeScale(reinterpret_cast<uint8_t*>(bank_addr));
    }
    // Attack banks aim from the gun hand (x/z only) instead of the body
    // centre; other banks keep the body origin.
    if (gun_source && bank_hdr[0] == BANK_CAT_ATTACK &&
        vrmod::mech_hand_origin_valid[1]) {
        origin[0] = vrmod::mech_hand_origin[1][0];
        origin[2] = vrmod::mech_hand_origin[1][2];
    }

    int inserted = original(bank, edx, candidate, owner, scratch);
    origin[0] = saved_ox;
    origin[2] = saved_oz;
    *yaw_cos = saved_cos;
    *vert_cos = saved_vert;
    *facing = saved;

    // Dual mechguns: the left copy pass, on the first live attack bank
    // only (the others share its geometry).
    last_left_ins = -1;
    if (gun_source && vrmod::mech_dual_active && fin_installed &&
        bank_hdr[0] == BANK_CAT_ATTACK) {
        const uintptr_t tgt =
            *reinterpret_cast<const uintptr_t*>(owner + ENT_TARGETING_OFF);
        const int idx = BankIndex(bank_addr, tgt);
        bool first = idx >= 0;
        for (int i = 0; first && i < idx; i++)
            if (BankLive(BankAt(tgt, i), BANK_CAT_ATTACK))
                first = false;
        if (first)
            LeftCopyTest(bank_addr, edx, candidate, owner);
    }

    return inserted;
}


// Write left candidates into a live attack bank: all of them (into an
// empty bank) or one requested record appended beside the right set.
inline void FeedBank(uintptr_t bank, const Cand* list, int n) {
    int32_t* count = reinterpret_cast<int32_t*>(bank + BANK_COUNT_OFF);
    float* maxd2 = reinterpret_cast<float*>(bank + BANK_MAXD2_OFF);
    for (int i = 0; i < n; i++) {
        int slot = *count;
        if (slot < 0)
            slot = 0;
        if (slot >= BANK_RECORD_CAP) {
            // Full: replace the farthest if ours is nearer (the test's
            // own worst-replacement rule).
            int worst = 0;
            float wd = -1.0f;
            for (int r = 0; r < BANK_RECORD_CAP; r++) {
                const float d = *reinterpret_cast<const float*>(
                    bank + BANK_RECORDS_OFF + (uintptr_t)r * 8 + 4);
                if (d > wd) {
                    wd = d;
                    worst = r;
                }
            }
            if (wd <= list[i].d2)
                continue;
            slot = worst;
        } else {
            *count = slot + 1;
        }
        uint8_t* rec = reinterpret_cast<uint8_t*>(bank + BANK_RECORDS_OFF +
                                                  (uintptr_t)slot * 8);
        *reinterpret_cast<uint16_t*>(rec) = list[i].id;
        *reinterpret_cast<int16_t*>(rec + 2) = list[i].part;
        *reinterpret_cast<float*>(rec + 4) = list[i].d2;
        if (list[i].d2 > *maxd2)
            *maxd2 = list[i].d2;
    }
}

inline void __fastcall FinalizerHook(void* driver, void* edx) {
    const uintptr_t d = reinterpret_cast<uintptr_t>(driver);
    uintptr_t entity = 0;
    uintptr_t tgt = 0;
    if (d != 0 && cached_entity != 0) {
        entity = *reinterpret_cast<const uintptr_t*>(d + DRIVER_ENTITY_OFF);
        if (entity == cached_entity)
            tgt = *reinterpret_cast<const uintptr_t*>(entity + ENT_TARGETING_OFF);
    }
    if (tgt == 0) {
        original_fin(driver, edx);
        return;
    }
    // Snapshot aim_cands before the commit.
    {
        aim_cands_n = 0;
        aim_cands_source = aim_source;
        const int first = FirstAttackBank(tgt);
        if (first >= 0 && aim_valid) {
            const uintptr_t bank = BankAt(tgt, first);
            int n = *reinterpret_cast<const int32_t*>(bank + BANK_COUNT_OFF);
            if (n < 0)
                n = 0;
            if (n > BANK_RECORD_CAP)
                n = BANK_RECORD_CAP;
            for (int i = 0; i < n; i++) {
                const uint8_t* rec = reinterpret_cast<const uint8_t*>(
                    bank + BANK_RECORDS_OFF + (uintptr_t)i * 8);
                const uint16_t id = *reinterpret_cast<const uint16_t*>(rec);
                const float d2 = *reinterpret_cast<const float*>(rec + 4);
                if (id == 0xFFFF || !(d2 == d2))
                    continue;
                AccInsert(aim_cands, aim_cands_n, id,
                          *reinterpret_cast<const int16_t*>(rec + 2), d2);
            }
        }
    }
    DualState s;
    s.active = vrmod::mech_dual_active;
    if (s.active) {
        const int first = FirstAttackBank(tgt);
        if (first >= 0) {
            const uintptr_t bank = BankAt(tgt, first);
            int n = *reinterpret_cast<const int32_t*>(bank + BANK_COUNT_OFF);
            if (n < 0)
                n = 0;
            if (n > BANK_RECORD_CAP)
                n = BANK_RECORD_CAP;
            for (int i = 0; i < n; i++) {
                const uint8_t* rec = reinterpret_cast<const uint8_t*>(
                    bank + BANK_RECORDS_OFF + (uintptr_t)i * 8);
                const uint16_t id = *reinterpret_cast<const uint16_t*>(rec);
                if (id == 0xFFFF)
                    continue;
                s.right[s.right_count++] = {
                    id, *reinterpret_cast<const int16_t*>(rec + 2),
                    *reinterpret_cast<const float*>(rec + 4)};
            }
            s.left_count = acc_left_n;
            for (int i = 0; i < acc_left_n; i++)
                s.left[i] = acc_left[i];
            s.left_valid = s.left_count > 0;
            // The hand-off.
            if (vrmod::config.mechgun_union && s.left_count > 0) {
                const Cand* feed = nullptr;
                int feed_n = 0;
                if (s.right_count == 0) {
                    feed = s.left;
                    feed_n = s.left_count;
                    s.union_fed = 1;
                } else {
                    const uint16_t req_id =
                        *reinterpret_cast<const uint16_t*>(tgt + TGT_REQ_ID_OFF);
                    const int16_t req_part =
                        *reinterpret_cast<const int16_t*>(tgt + TGT_REQ_PART_OFF);
                    if (req_id != 0xFFFF &&
                        !InListPair(s.right, s.right_count, req_id, req_part)) {
                        for (int i = 0; i < s.left_count; i++) {
                            if (s.left[i].id == req_id &&
                                s.left[i].part == req_part) {
                                feed = &s.left[i];
                                feed_n = 1;
                                s.union_fed = 2;
                                break;
                            }
                        }
                    }
                }
                if (feed != nullptr)
                    for (int i = 0; i < TGT_BANK_COUNT; i++)
                        if (BankLive(BankAt(tgt, i), BANK_CAT_ATTACK))
                            FeedBank(BankAt(tgt, i), feed, feed_n);
            }
        }
    }
    acc_left_n = 0;

    original_fin(driver, edx);

    const uint16_t cur = *reinterpret_cast<const uint16_t*>(tgt + TGT_CUR_ID_OFF);
    if (cur != 0xFFFF) {
        s.cur_valid = true;
        s.cur_id = cur;
        s.cur_part = *reinterpret_cast<const int16_t*>(tgt + TGT_CUR_PART_OFF);
        s.cur_cat = *reinterpret_cast<const uint8_t*>(tgt + TGT_CUR_CAT_OFF);
        if (s.active && s.cur_cat == BANK_CAT_ATTACK) {
            s.right_owns = InList(s.right, s.right_count, cur);
            s.left_owns = InList(s.left, s.left_count, cur);
        }
    }
    s.valid = true;
    dual = s;
}

// Called from the first BeginScene; behavior is gated in OnFrame / Hook.
inline void Install() {
    static bool tried = false;
    if (tried)
        return;
    tried = true;

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        probe::Log("targetaim: MH_Initialize failed (%d)", (int)status);
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(CANDIDATE_TEST_ADDR),
                      reinterpret_cast<void*>(&Hook),
                      reinterpret_cast<void**>(&original)) != MH_OK) {
        probe::Log("targetaim: MH_CreateHook failed");
        return;
    }
    if (MH_EnableHook(reinterpret_cast<void*>(CANDIDATE_TEST_ADDR)) != MH_OK) {
        probe::Log("targetaim: MH_EnableHook failed");
        return;
    }
    installed = true;
    probe::Log("targetaim: candidate-test hook installed at 0x%08X",
               (unsigned)CANDIDATE_TEST_ADDR);
    // Optional: without the finalizer hook only the left mechgun lock is
    // lost (dual.valid stays false).
    if (MH_CreateHook(reinterpret_cast<void*>(FINALIZER_ADDR),
                      reinterpret_cast<void*>(&FinalizerHook),
                      reinterpret_cast<void**>(&original_fin)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(FINALIZER_ADDR)) == MH_OK) {
        fin_installed = true;
        probe::Log("targetaim: finalizer hook installed at 0x%08X",
                   (unsigned)FINALIZER_ADDR);
        diag::Log("targetaim: finalizer hook installed (dual-mechgun left "
                  "lock via the game's own test)");
    } else {
        probe::Log("targetaim: finalizer hook FAILED - left mechgun lock off");
        diag::Log("targetaim: finalizer hook FAILED - left mechgun lock off");
    }
}

}  // namespace targetaim
