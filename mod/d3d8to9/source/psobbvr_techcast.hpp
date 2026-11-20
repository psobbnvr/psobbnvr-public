#pragma once

// Spellcasting: no facing snap on cast, unlocked aimed casts along the
// left controller's ray, plus a trace. Arm-then-swing casting is in
// psobbvr_controller.hpp, the left hand's target pick in
// psobbvr_targetaim.hpp.
//
// A palette type-3 tech writes its id to [entity+0x464] and starts action
// mode 8. The mode-8 setup (0x6A03F0) stores the target's position at
// +0x820 (the cast aim point) and the yaw toward it at +0x3FC; nothing
// reads +0x820 after that. The tech tick (0x6A00AC, case 1 of the mode-8
// router 0x69FE2C) then chases the facing toward +0x3FC each tick
// (0x7A8CB8), snapping the character round.
// Every cast seen runs type 3 / mode 8; type 5 / mode 9 is unexplored.
//
// With cast_facing_snap=0, TickHook sets +0x3FC to the current facing for
// the local player during the call and restores it after, so the chase
// does nothing. Cosmetic only: a locked cast flies to its attack pair's
// target whatever the facing.
//
// The technique effect launcher 0x6E0780 runs at the cast commit for every
// caster and, for Foie, Barta, Gibarta and Megid, calls an unaimed launch
// variant (direction from the facing yaw alone, no pitch) when the attack
// pair list is empty, or a point variant (direction = point - origin in
// 3D) when a target is locked. With cast_aim_point=1, AimHook replaces the
// local player's unaimed launch with the point variant, the point
// cast_aim_point_dist along the left aim direction from the game's origin
// (the line from the shoulder through the hand), so the spell flies parallel
// to the arm, pitch included. Locked casts and every other caster run stock.

#include <cmath>
#include <cstdint>

#include "MinHook.h"
#include "psobbvr_armray.hpp"
#include "psobbvr_gamecam.hpp"
#include "psobbvr_log.hpp"
#include "psobbvr_probe.hpp"
#include "psobbvr_vr.hpp"

namespace techcast {

// The tech tick (thiscall on the entity, no stack args).
constexpr uintptr_t TECH_TICK_ADDR = 0x006A00AC;

constexpr uintptr_t ENT_ACTION_MODE_OFF = 0x32E;   // word: action mode
constexpr uintptr_t ENT_ACTION_STAGE_OFF = 0x330;  // word: stage in mode
constexpr uintptr_t ENT_TECH_ID8_OFF = 0x464;      // word: type-3 tech id
constexpr uintptr_t ENT_TECH_ID9_OFF = 0x4E6;      // word: type-5 tech id
constexpr uintptr_t ENT_FACING_OFF = 0x60;         // dword: facing (BAMS16)
constexpr uintptr_t ENT_TARGET_YAW_OFF = 0x3FC;    // dword: chase target yaw
constexpr uintptr_t ENT_CAST_AIM_OFF = 0x820;      // 3 floats: cast aim point
constexpr uintptr_t ENT_PAL_LATCH_OFF = 0x354;     // bytes: fired {type,param}
constexpr uintptr_t ENT_ATK_PAIRS_OFF = 0x480;     // words: pair 0 {ID, part}
constexpr uintptr_t ENT_TARGETING_OFF = 0x45C;     // ptr: targeting object
constexpr uintptr_t TGT_CURRENT_ID_OFF = 0x108C;   // word: current target ID
constexpr uintptr_t TGT_REQUEST_ID_OFF = 0x1098;   // word: requested (lock) ID

// Launch variants called by the technique effect launcher 0x6E0780 (cdecl,
// four dword args, caller cleans). The unaimed one takes (entity, origin
// xyz, facing yaw BAMS, level) and builds its direction from the yaw
// through the base constructor 0x6EFE50; the point one takes (entity,
// origin xyz, point xyz, level) and builds it from point - origin through
// 0x6F0138. Both allocate the same projectile class.
constexpr uintptr_t FOIE_UNAIMED_ADDR = 0x006E2F20;     // Foie (tech 0)
constexpr uintptr_t FOIE_POINT_ADDR = 0x006E3408;
constexpr uintptr_t BARTA_UNAIMED_ADDR = 0x006DD7E4;    // Barta (tech 3)
constexpr uintptr_t BARTA_POINT_ADDR = 0x006DDDAC;
constexpr uintptr_t GIBARTA_UNAIMED_ADDR = 0x006E38E0;  // Gibarta (tech 4)
constexpr uintptr_t GIBARTA_POINT_ADDR = 0x006E4058;
constexpr uintptr_t MEGID_UNAIMED_ADDR = 0x006E9848;    // Megid (tech 0x12)
constexpr uintptr_t MEGID_POINT_ADDR = 0x006E9E14;


// __fastcall stands in for the game's thiscall (edx unused); no stack
// args, so the calling conventions match at the return too.
using TickFn = void(__fastcall*)(void* entity, void* edx);
inline TickFn tick_original = nullptr;
inline bool installed = false;

// Suppresses the cast facing chase for the local player while the mod
// drives the view; every other entity runs stock. +0x3FC is restored
// after the call for any other reader.
inline void __fastcall TickHook(void* entity, void* edx) {
    const uintptr_t e = reinterpret_cast<uintptr_t>(entity);
    if (vrmod::config.cast_facing_snap || e == 0 ||
        e != gamecam::ResolveEntity() || !gamecam::DrivesView()) {
        tick_original(entity, edx);
        return;
    }
    int32_t* target_yaw = reinterpret_cast<int32_t*>(e + ENT_TARGET_YAW_OFF);
    const int32_t saved = *target_yaw;
    *target_yaw = *reinterpret_cast<const int32_t*>(e + ENT_FACING_OFF);
    tick_original(entity, edx);
    *target_yaw = saved;
}

// The launch variants' shapes; the return value is passed through as is.
using UnaimedFn = uintptr_t(__cdecl*)(void* entity, const float* origin,
                                      uint32_t yaw, int level);
using PointFn = uintptr_t(__cdecl*)(void* entity, const float* origin,
                                    const float* point, int level);

struct AimSite {
    uintptr_t unaimed_addr;
    uintptr_t point_addr;
    const char* name;
};
constexpr AimSite kAimSites[] = {
    {FOIE_UNAIMED_ADDR, FOIE_POINT_ADDR, "foie"},
    {BARTA_UNAIMED_ADDR, BARTA_POINT_ADDR, "barta"},
    {GIBARTA_UNAIMED_ADDR, GIBARTA_POINT_ADDR, "gibarta"},
    {MEGID_UNAIMED_ADDR, MEGID_POINT_ADDR, "megid"},
};
constexpr int kAimSiteCount = (int)(sizeof(kAimSites) / sizeof(kAimSites[0]));
inline UnaimedFn aim_original[kAimSiteCount] = {};
inline int aim_installed = 0;

// The point cast_aim_point_dist along the left aim direction (the arm line
// or the controller's pointing, see psobbvr_armray.hpp) from the game's
// origin, for the local player's casts while the mod drives the view. False
// = run the cast stock. source / weight say how the direction was built.
inline bool LeftRayPoint(void* entity, const float* origin, float point[3],
                         const char*& source, float& weight) {
    const uintptr_t e = reinterpret_cast<uintptr_t>(entity);
    if (!vrmod::config.cast_aim_point || e == 0 || origin == nullptr ||
        e != gamecam::ResolveEntity() || !gamecam::DrivesView())
        return false;
    armray::Result arm;
    if (!armray::LeftAim(arm))
        return false;
    const float k = vrmod::config.cast_aim_point_dist;
    point[0] = origin[0] + arm.dir[0] * k;
    point[1] = origin[1] + arm.dir[1] * k;
    point[2] = origin[2] + arm.dir[2] * k;
    source = arm.source;
    weight = arm.weight;
    return true;
}

// Stands in for an unaimed launch variant: the local player's unlocked
// cast goes to the point variant with a point on the left ray instead.
// Locked casts never come here (the launcher calls the point variant).
template <int N>
uintptr_t __cdecl AimHook(void* entity, const float* origin, uint32_t yaw,
                          int level) {
    float point[3];
    const char* source = "";
    float weight = 0.0f;
    if (!LeftRayPoint(entity, origin, point, source, weight))
        return aim_original[N](entity, origin, yaw, level);
    diag::Log("castaim: %s -> %.1f/%.1f/%.1f (%s, shoulder weight %.2f)",
              kAimSites[N].name, point[0], point[1], point[2], source,
              weight);
    return reinterpret_cast<PointFn>(kAimSites[N].point_addr)(entity, origin,
                                                             point, level);
}

inline void InstallAimHook(int n, void* hook) {
    const uintptr_t addr = kAimSites[n].unaimed_addr;
    if (MH_CreateHook(reinterpret_cast<void*>(addr), hook,
                      reinterpret_cast<void**>(&aim_original[n])) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(addr)) != MH_OK) {
        probe::Log("techcast: %s launch hook failed", kAimSites[n].name);
        diag::Log("techcast: %s launch hook failed", kAimSites[n].name);
        return;
    }
    aim_installed++;
    diag::Log("techcast: %s launch hook installed at 0x%08X",
              kAimSites[n].name, (unsigned)addr);
}

// Called from the first BeginScene; behavior is gated per call in TickHook
// and AimHook.
inline void Install() {
    static bool tried = false;
    if (tried)
        return;
    tried = true;

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        probe::Log("techcast: MH_Initialize failed (%d)", (int)status);
        diag::Log("techcast: MH_Initialize failed (%d)", (int)status);
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(TECH_TICK_ADDR),
                      reinterpret_cast<void*>(&TickHook),
                      reinterpret_cast<void**>(&tick_original)) != MH_OK) {
        probe::Log("techcast: MH_CreateHook failed");
    } else if (MH_EnableHook(reinterpret_cast<void*>(TECH_TICK_ADDR)) !=
               MH_OK) {
        probe::Log("techcast: MH_EnableHook failed");
    } else {
        installed = true;
        probe::Log("techcast: tech tick-machine hook installed at 0x%08X",
                   (unsigned)TECH_TICK_ADDR);
    }
    static_assert(kAimSiteCount == 4, "one AimHook instance per site");
    InstallAimHook(0, reinterpret_cast<void*>(&AimHook<0>));
    InstallAimHook(1, reinterpret_cast<void*>(&AimHook<1>));
    InstallAimHook(2, reinterpret_cast<void*>(&AimHook<2>));
    InstallAimHook(3, reinterpret_cast<void*>(&AimHook<3>));
}

// The trace log; nothing else runs per frame.
inline void OnFrame() {
}

}  // namespace techcast
