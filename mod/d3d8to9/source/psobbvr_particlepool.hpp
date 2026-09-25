#pragma once
// Which of the game's particle pools is drawing.
//
// The game has 108 fixed particle pools (one per record of the 40-byte
// table at 0xA101C0; pool pointers at 0xAAEE20, built at startup by
// 0x8013BC). Two draw methods (thiscall on the pool) issue one projected
// screen quad per particle through the 2D drawer 0x82B440:
//   0x801DF8  vtable 0xB473B8 slot 2 - untextured additive quads (site
//             0x802015: footstep dust, the telepipe beam and sparkles)
//   0x802114  vtable 0xB473F0 slot 2 - textured alpha quads (site
//             0x802412: Forest grass)
// The hooks publish the pool index during the draw, so the world-sprite
// route can treat a pool's quads by identity ([vr] sprite_upright_pools)
// and the census can name the pool.
#include <cstdint>
#include <windows.h>
#include "MinHook.h"
#include "psobbvr_log.hpp"
#include "psobbvr_stereo.hpp"

namespace particlepool {

constexpr uintptr_t POOL_ARRAY = 0xAAEE20;     // 108 pool object pointers
constexpr int POOL_COUNT = 108;
constexpr uintptr_t DRAW_ADDITIVE = 0x801DF8;  // pool draw, untextured additive
constexpr uintptr_t DRAW_ALPHA = 0x802114;     // pool draw, textured alpha

using DrawFn = void(__fastcall*)(void* self, void* edx);
inline DrawFn original_additive = nullptr;
inline DrawFn original_alpha = nullptr;

inline int IndexOf(void* pool) {
    const auto* arr = reinterpret_cast<void* const*>(POOL_ARRAY);
    if (IsBadReadPtr(arr, sizeof(void*) * POOL_COUNT))
        return -1;
    for (int i = 0; i < POOL_COUNT; i++)
        if (arr[i] == pool)
            return i;
    return -1;
}

inline void __fastcall HookAdditive(void* self, void* edx) {
    const int prev = stereo::current_particle_pool;
    stereo::current_particle_pool = IndexOf(self);
    original_additive(self, edx);
    stereo::current_particle_pool = prev;
}

inline void __fastcall HookAlpha(void* self, void* edx) {
    const int prev = stereo::current_particle_pool;
    stereo::current_particle_pool = IndexOf(self);
    original_alpha(self, edx);
    stereo::current_particle_pool = prev;
}

inline void Install() {
    static bool tried = false;
    if (tried)
        return;
    tried = true;
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        diag::Log("particlepool: MH_Initialize failed (%d)", (int)status);
        return;
    }
    status = MH_CreateHook(reinterpret_cast<void*>(DRAW_ADDITIVE), reinterpret_cast<void*>(&HookAdditive),
                           reinterpret_cast<void**>(&original_additive));
    if (status == MH_OK)
        status = MH_CreateHook(reinterpret_cast<void*>(DRAW_ALPHA), reinterpret_cast<void*>(&HookAlpha),
                               reinterpret_cast<void**>(&original_alpha));
    if (status == MH_OK)
        status = MH_EnableHook(MH_ALL_HOOKS);
    if (status != MH_OK) {
        diag::Log("particlepool: hook install failed (%d)", (int)status);
        return;
    }
    diag::Log("particlepool: pool draw hooks installed at 0x%08X / 0x%08X",
              (unsigned)DRAW_ADDITIVE, (unsigned)DRAW_ALPHA);
}

}  // namespace particlepool
