#pragma once

// Widens the game's own field of view while VR drives, so its CPU-side
// culling covers the wider headset view instead of the narrow flat cone.
//
// The projection builder 0x0082EBC8 takes its FOV as the immediate float
// 1.1333333f (0x3F911112) pushed at nine call sites; a smaller divisor
// gives a wider angle. The cull cone (rebuilt by 0x004D4374) and the
// screen-projection scale at 0xAF02D8 all derive from it, so scaling it
// keeps the game's math consistent. The rendered view is unaffected: the
// per-eye projections replace the game's at SetTransform. The original
// bytes are restored whenever VR is not driving.
//
// [vr] cull_fov_scale / cull_fov_scale_town (< 1 = wider). Written on the game thread (BeginScene).

#include <cstdint>
#include <cstring>
#include <windows.h>

#include "psobbvr_gamecam.hpp"
#include "psobbvr_vr.hpp"

namespace cullfov {

// The 4-byte float inside each "68 12 11 91 3f" push.
constexpr float ORIGINAL_DIVISOR = 1.1333333f;
constexpr uintptr_t SITES[] = {
    0x004D2AC3, 0x004D3B80, 0x004D411C, 0x004D4247,  // camera-update variants
    0x004EB41E,                                       // secondary view
    0x00502526, 0x00502886,                           // game-mode setups
    0x0078DE32,                                       // startup path
    0x00804A5D,                                       // engine helper
};

// Scale currently written into the code; 0 = original bytes in place.
inline float applied_scale = 0.0f;

inline void WriteAll(float divisor) {
    for (uintptr_t site : SITES) {
        DWORD old_protect;
        if (VirtualProtect(reinterpret_cast<void*>(site), 4, PAGE_EXECUTE_READWRITE, &old_protect)) {
            memcpy(reinterpret_cast<void*>(site), &divisor, 4);
            VirtualProtect(reinterpret_cast<void*>(site), 4, old_protect, &old_protect);
        }
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
}

// Called every frame from BeginScene; writes only on a change.
inline void OnFrame(bool takeover_driving) {
    float want = 0.0f;
    if (takeover_driving) {
        float s = vrmod::config.cull_fov_scale;
        // Town (floor 0, Pioneer 2) is heavy and has no combat, so it has
        // its own scale (0.3 costs ~25% of the game tick there).
        const float town = vrmod::config.cull_fov_scale_town;
        if (town > 0.0f) {
            const uintptr_t entity = gamecam::ResolveEntity();
            if (entity != 0 &&
                diag::Accessible(entity + gamecam::ENTITY_FLOOR_OFFSET, 4, false) &&
                *reinterpret_cast<const uint32_t*>(entity + gamecam::ENTITY_FLOOR_OFFSET) == 0)
                s = town;
        }
        if (s > 0.0f && s < 1.0f)
            want = s;
    }
    if (want == applied_scale)
        return;
    WriteAll(want == 0.0f ? ORIGINAL_DIVISOR : ORIGINAL_DIVISOR * want);
    applied_scale = want;
}

} // namespace cullfov
