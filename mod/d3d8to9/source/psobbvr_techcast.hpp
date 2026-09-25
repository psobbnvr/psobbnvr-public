#pragma once

// Spellcasting: no facing snap on cast, plus a trace. Arm-then-swing
// casting is in psobbvr_controller.hpp, left-hand aim in
// psobbvr_targetaim.hpp.
//
// A palette type-3 tech writes its id to [entity+0x464] and starts action
// mode 8. The mode-8 setup (0x6A03F0) stores the target's position at
// +0x820 (the cast aim point) and the yaw toward it at +0x3FC; the tech
// tick (0x6A00AC, case 1 of the mode-8 router 0x69FE2C) then chases the
// facing toward +0x3FC each tick (0x7A8CB8), snapping the character round.
// Every cast seen runs type 3 / mode 8; type 5 / mode 9 is unexplored.
//
// With cast_facing_snap=0, TickHook sets +0x3FC to the current facing for
// the local player during the call and restores it after, so the chase
// does nothing. Cosmetic only: the cast targets through the attack pairs
// and aim point, not the facing.
//
// 'techtrace <n>' logs one line per frame in modes 8/9 (stage, tech ids,
// facing vs target yaw, aim point, palette latch, pair 0, target IDs)
// plus mode entry/exit. 'castsnap <0|1>' toggles the suppression.

#include <cstdint>

#include "MinHook.h"
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

// Called from the first BeginScene; behavior is gated per call in TickHook.
inline void Install() {
    static bool tried = false;
    if (tried)
        return;
    tried = true;

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        probe::Log("techcast: MH_Initialize failed (%d)", (int)status);
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(TECH_TICK_ADDR),
                      reinterpret_cast<void*>(&TickHook),
                      reinterpret_cast<void**>(&tick_original)) != MH_OK) {
        probe::Log("techcast: MH_CreateHook failed");
        return;
    }
    if (MH_EnableHook(reinterpret_cast<void*>(TECH_TICK_ADDR)) != MH_OK) {
        probe::Log("techcast: MH_EnableHook failed");
        return;
    }
    installed = true;
    probe::Log("techcast: tech tick-machine hook installed at 0x%08X",
               (unsigned)TECH_TICK_ADDR);
}

// The 'techtrace' log; nothing else runs per frame.
inline void OnFrame() {
}

}  // namespace techcast
