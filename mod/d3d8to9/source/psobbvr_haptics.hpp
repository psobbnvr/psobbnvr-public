// psobbvr_haptics.hpp - combat haptics, via VRInterface::HapticPulse.
// Overlapping pulses restart the motor and read as one longer buzz.
//
// Hit pulse (weapon hand) and hurt pulse (both hands), from a hook on the
// per-hit damage applier 0x7732B8 (the vtbl+0x5C base; it owns the damage
// popup calls and the HP subtract). Thiscall on the victim, the attacker
// entity is the first stack arg (id word +0x1C). The player's own +0x5C
// override (0x68C330) forwards to it, so one hook sees our hits and hits
// on us. The vtbl+0x60 TakeHit (0x773AFC) is hooked too but never seen
// firing; the 30 ms refractory absorbs a double-fire. Guns get no hit
// pulse: they buzz on firing (psobbvr_gunfire.hpp FireHaptic, or the
// trigger-pull mode in psobbvr_controller.hpp).
//
// Config [vr]: hit_haptic / hurt_haptic with their _amp / _s pairs.

#pragma once

#include <cstdint>

#include "MinHook.h"
#include "psobbvr_controller.hpp"
#include "psobbvr_gamecam.hpp"
#include "psobbvr_log.hpp"
#include "psobbvr_probe.hpp"
#include "psobbvr_vr.hpp"
#include "psobbvr_weapongrip.hpp"

namespace haptics {

// TakeHit base (vtbl+0x60; thiscall, 6 stack args, ret 0x18, float
// return). Never seen firing; hooked as coverage.
constexpr uintptr_t TAKE_HIT_ADDR = 0x00773AFC;
// Per-hit damage applier, the vtbl+0x5C base (thiscall on the victim,
// args (attacker entity, a2), ret 8, float return). The player's +0x5C
// override 0x68C330 forwards here, so it covers hits by and on us.
constexpr uintptr_t APPLY_HIT_ADDR = 0x007732B8;
// React-only vtbl+0x5C bases: containers and other no-HP objects play a
// hit effect and apply no damage, so the applier hook never sees them.
// A connecting swing calls them with the same args, which gives contact
// feedback. 0x64D9E4 is the Forest box class (vtable 0xB30A60);
// 0x62E86C and 0x639E90 are the same template for other areas (untested).
// The effect-less stub 0x626D64 (likely intangible triggers) is not hooked.
constexpr uintptr_t REACT_HIT_ADDRS[3] = {0x0064D9E4, 0x0062E86C,
                                          0x00639E90};
constexpr uintptr_t ENT_ID_OFF = 0x1C;          // word: entity id

using TakeHitFn = float(__fastcall*)(void* victim, void* edx, void* attacker,
                                     int damage_type, float base_damage,
                                     float* hit_pos, int tech_idx, int x);
using ApplyHitFn = float(__fastcall*)(void* victim, void* edx, void* attacker,
                                      int a2);

inline TakeHitFn original_takehit = nullptr;
inline ApplyHitFn original_applyhit = nullptr;
inline ApplyHitFn original_react[3] = {nullptr, nullptr, nullptr};

// Minimum gap between hit pulses: a sweep hitting five enemies in one
// tick gives one buzz, not five.
constexpr double HIT_PULSE_REFRACTORY_S = 0.03;
inline LONGLONG hit_pulse_qpc = 0;
inline LONGLONG hurt_pulse_qpc = 0;
inline LONGLONG qpf = 0;

inline bool Refractory(LONGLONG& last_qpc, double min_s) {
    if (qpf == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpf = f.QuadPart;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (last_qpc != 0 &&
        double(now.QuadPart - last_qpc) / double(qpf) < min_s)
        return false;
    last_qpc = now.QuadPart;
    return true;
}

// Shared by all hooks: weapon-hand pulse when our hit lands on someone
// else (not with a gun; bare hands count as melee), both hands when we
// are hit.
inline void PulseGates(uintptr_t ours, uintptr_t v, uintptr_t a) {
    const auto& c = vrmod::config;
    if (ours == 0 || !gamecam::Enabled())
        return;
    if (a == ours && v != ours && c.hit_haptic &&
        !weapongrip::current_is_gun &&
        Refractory(hit_pulse_qpc, HIT_PULSE_REFRACTORY_S))
        vrmod::Get()->HapticPulse(1, c.hit_haptic_s, c.hit_haptic_amp);
    if (v == ours && c.hurt_haptic &&
        Refractory(hurt_pulse_qpc, HIT_PULSE_REFRACTORY_S)) {
        vrmod::Get()->HapticPulse(0, c.hurt_haptic_s, c.hurt_haptic_amp);
        vrmod::Get()->HapticPulse(1, c.hurt_haptic_s, c.hurt_haptic_amp);
    }
}

inline float __fastcall TakeHitHook(void* victim, void* edx, void* attacker,
                                    int damage_type, float base_damage,
                                    float* hit_pos, int tech_idx, int x) {
    const float ret = original_takehit(victim, edx, attacker, damage_type,
                                       base_damage, hit_pos, tech_idx, x);
    const uintptr_t ours = gamecam::ResolveEntity();
    const uintptr_t v = reinterpret_cast<uintptr_t>(victim);
    const uintptr_t a = reinterpret_cast<uintptr_t>(attacker);
    PulseGates(ours, v, a);
    return ret;
}

// Every hit that lands in play passes through here (see APPLY_HIT_ADDR).
inline float __fastcall ApplyHitHook(void* victim, void* edx, void* attacker,
                                     int a2) {
    const uintptr_t ours = gamecam::ResolveEntity();
    const uintptr_t v = reinterpret_cast<uintptr_t>(victim);
    const uintptr_t a = reinterpret_cast<uintptr_t>(attacker);
    // Our hit's victim while the applier runs, for gunfire's popup merge
    // (the damage popup is spawned from inside).
    vrmod::apply_scope_victim = (ours != 0 && a == ours && v != ours) ? v : 0;
    const float ret = original_applyhit(victim, edx, attacker, a2);
    vrmod::apply_scope_victim = 0;
    PulseGates(ours, v, a);
    return ret;
}

// A swing connecting with a no-HP object (see REACT_HIT_ADDRS): a
// weapon-hand contact buzz through the shared gates.
inline float ReactHitCommon(int idx, void* victim, void* edx,
                            void* attacker, int a2) {
    const float ret = original_react[idx](victim, edx, attacker, a2);
    const uintptr_t ours = gamecam::ResolveEntity();
    const uintptr_t v = reinterpret_cast<uintptr_t>(victim);
    const uintptr_t a = reinterpret_cast<uintptr_t>(attacker);
    PulseGates(ours, v, a);
    return ret;
}

inline float __fastcall ReactHit0(void* v, void* edx, void* a, int a2) {
    return ReactHitCommon(0, v, edx, a, a2);
}
inline float __fastcall ReactHit1(void* v, void* edx, void* a, int a2) {
    return ReactHitCommon(1, v, edx, a, a2);
}
inline float __fastcall ReactHit2(void* v, void* edx, void* a, int a2) {
    return ReactHitCommon(2, v, edx, a, a2);
}

// Called from the first BeginScene; behavior is gated per call.
inline void Install() {
    static bool tried = false;
    if (tried)
        return;
    tried = true;

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        probe::Log("haptics: MH_Initialize failed (%d)", (int)status);
        diag::Log("haptics: MH_Initialize failed (%d)", (int)status);
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(TAKE_HIT_ADDR),
                      reinterpret_cast<void*>(&TakeHitHook),
                      reinterpret_cast<void**>(&original_takehit)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(TAKE_HIT_ADDR)) != MH_OK) {
        probe::Log("haptics: TakeHit hook failed");
        diag::Log("haptics: TakeHit hook failed");
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(APPLY_HIT_ADDR),
                      reinterpret_cast<void*>(&ApplyHitHook),
                      reinterpret_cast<void**>(&original_applyhit)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(APPLY_HIT_ADDR)) != MH_OK) {
        probe::Log("haptics: ApplyHit hook failed");
        diag::Log("haptics: ApplyHit hook failed");
        return;
    }
    void* react_hooks[3] = {reinterpret_cast<void*>(&ReactHit0),
                            reinterpret_cast<void*>(&ReactHit1),
                            reinterpret_cast<void*>(&ReactHit2)};
    for (int i = 0; i < 3; i++) {
        if (MH_CreateHook(reinterpret_cast<void*>(REACT_HIT_ADDRS[i]),
                          react_hooks[i],
                          reinterpret_cast<void**>(&original_react[i])) !=
                MH_OK ||
            MH_EnableHook(reinterpret_cast<void*>(REACT_HIT_ADDRS[i])) !=
                MH_OK) {
            probe::Log("haptics: ReactHit%d hook failed (0x%08X)", i,
                       (unsigned)REACT_HIT_ADDRS[i]);
            diag::Log("haptics: ReactHit%d hook failed (0x%08X)", i,
                      (unsigned)REACT_HIT_ADDRS[i]);
            return;
        }
    }
    probe::Log("haptics: TakeHit + ApplyHit + 3 ReactHit hooks installed "
               "(0x%08X, 0x%08X, 0x%08X/0x%08X/0x%08X)",
               (unsigned)TAKE_HIT_ADDR, (unsigned)APPLY_HIT_ADDR,
               (unsigned)REACT_HIT_ADDRS[0], (unsigned)REACT_HIT_ADDRS[1],
               (unsigned)REACT_HIT_ADDRS[2]);
    diag::Log("haptics: TakeHit + ApplyHit + 3 ReactHit hooks installed "
              "(0x%08X, 0x%08X, 0x%08X/0x%08X/0x%08X)",
              (unsigned)TAKE_HIT_ADDR, (unsigned)APPLY_HIT_ADDR,
              (unsigned)REACT_HIT_ADDRS[0], (unsigned)REACT_HIT_ADDRS[1],
              (unsigned)REACT_HIT_ADDRS[2]);
}

}  // namespace haptics
