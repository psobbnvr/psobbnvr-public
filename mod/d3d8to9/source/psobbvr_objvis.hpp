// Removes screen-edge object culling while VR drives the view, so objects
// at the edges of the wide headset view (and the held weapon and mag) do
// not vanish.
//
// World objects test visibility through shared helpers (thiscall; world
// position at this+0x38/0x3C/0x40, cached result word at this+0x1E, also
// returned in eax):
//   0x007BA468  (float far_limit, float radius)             ret 8
//   0x007BA638  (float far_limit, float radius, float arg3) ret 0xC
// Both transform into the internal camera's view space (0x835C3C), reject
// behind-camera and beyond far_limit, then project (0x82F09C) and test the
// screen edges (right <= [0xA48A4C] + margin [0x9ACB68]). The hooks keep
// only the range test (see InRange). The special paths
// (flag bit 0x400 at this+8, and [0xA16380]==1 = reuse the cached answer)
// defer to the original. psobbvr_cullfov.hpp does not cover these.
//
// Do not route other objects through the 0x400 family's distance-only
// helper 0x00502C78: its range at [0xA72A94] is set only for that family,
// and NPCs, doors and teleporters vanish.
//
// Some classes inline the edge test instead (floor items 0x5C5267,
// TObjCamera 0x64394C, ...). The held weapon and the mag use the
// base-class virtual and its raw-position entry (C and D below).
//
// [vr] object_vis_fix.
#pragma once

#include <cstdint>
#include <intrin.h>
#include <windows.h>
#include "MinHook.h"
#include "psobbvr_gamecam.hpp"
#include "psobbvr_log.hpp"
#include "psobbvr_probe.hpp"
#include "psobbvr_vr.hpp"

namespace objvis {

// The mag's update virtual (vtbl slot 1 = 0x5DC7C0): early-outs when
// [[this+0x1C]+0xF8]+0x33C has bit 0x200 (0x5DB444) or [[this+0x1C]+0x1B0]
// has bit 0x20. Otherwise the hover target is a bone position - profile
// [this+0x30], first dword = bone index (-1 = the owner's anchor at
// owner+0x300) - via the owner virtual vtbl+0x10C = 0x694320 (bone matrix
// [owner+0xE0] + index*64), and the position at this+0xB4..0xBC lerps
// toward it (0x5DC62C) unless [this+0x64] bit 2 (snap).
constexpr uintptr_t MAG_UPDATE = 0x005DC7C0;
using MagUpdateFn = void(__fastcall*)(void* self, void* edx);
inline MagUpdateFn original_mag_update = nullptr;

constexpr uintptr_t ONSCREEN_TEST_A = 0x007BA468;  // 2-arg screen test
constexpr uintptr_t ONSCREEN_TEST_B = 0x007BA638;  // 3-arg screen test
// The object base class's on-screen virtual (inherited by the floor-item
// and equipment family). Same shape as A but pure: returns 0/1, no cached
// flag, no special paths. The held weapon's path.
constexpr uintptr_t ONSCREEN_TEST_C = 0x005C5264;
// Its raw-position entry, straight into the core at 0x5C528C:
// cdecl(pos*, far, radius). The mag's draw gate (0x5DCFA4) calls it with
// [this+0x20]+0x300, far 400, radius 20 and skips the draw on a miss.
constexpr uintptr_t ONSCREEN_TEST_D = 0x005C5284;
constexpr uintptr_t WORLD_TO_VIEW = 0x00835C3C;    // cdecl(pos*, out3*) via internal camera
constexpr uintptr_t CACHE_REUSE_MODE = 0x00A16380; // ==1: tests return cached +0x1E
constexpr uintptr_t OBJ_FLAGS_OFFSET = 0x08;       // word; bit 0x400 = distance-only family
constexpr uintptr_t OBJ_POS_OFFSET = 0x38;         // world x,y,z floats
constexpr uintptr_t OBJ_VISIBLE_OFFSET = 0x1E;     // cached result word

using TransformFn = void(__cdecl*)(const float* pos, float* out);
// __fastcall matches the game's thiscall + callee-cleaned stack floats
// (edx is unused garbage).
using TestAFn = int(__fastcall*)(void* self, void* edx,
                                 float far_limit, float radius);
using TestBFn = int(__fastcall*)(void* self, void* edx,
                                 float far_limit, float radius, float arg3);

inline TestAFn original_a = nullptr;
inline TestBFn original_b = nullptr;
inline TestAFn original_c = nullptr;  // same (far_limit, radius) shape as A
using TestDFn = int(__cdecl*)(const float* pos, float far_limit, float radius);
inline TestDFn original_d = nullptr;

// True when the main path should be overridden; the special early paths
// (distance-only family, cache-reuse mode) always defer to the original.
inline bool WantsOverride(void* self) {
    if (!vrmod::config.object_vis_fix || !gamecam::DrivesView())
        return false;
    const uint16_t flags =
        *reinterpret_cast<const uint16_t*>(reinterpret_cast<uintptr_t>(self) + OBJ_FLAGS_OFFSET);
    if ((flags & 0x400) != 0)
        return false;
    if (*reinterpret_cast<const int*>(CACHE_REUSE_MODE) == 1)
        return false;
    return true;
}

// The originals' range test alone: no edge test, and no behind test (the
// originals drop objects at view depth > radius): a box's hit handling
// reads its visible flag, so a technique cast at a box behind you would
// otherwise not break it. View depth is negative in front of the camera;
// < -(far_limit + radius) = out of range.
inline int InRange(const float* pos, float far_limit, float radius) {
    float view[3] = {};
    reinterpret_cast<TransformFn>(WORLD_TO_VIEW)(pos, view);
    const float r = radius >= 0.0f ? radius : -radius;
    if (view[2] < -(far_limit + r))
        return 0;
    return 1;
}

// A/B: also write the cached visibility word, as the originals do.
inline int InRangeOnly(void* self, float far_limit, float radius) {
    const auto* pos =
        reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(self) + OBJ_POS_OFFSET);
    const int vis = InRange(pos, far_limit, radius);
    *reinterpret_cast<uint16_t*>(reinterpret_cast<uintptr_t>(self) + OBJ_VISIBLE_OFFSET) =
        static_cast<uint16_t>(vis);
    return vis;
}

inline int __fastcall HookA(void* self, void* edx, float far_limit, float radius) {
    if (!WantsOverride(self))
        return original_a(self, edx, far_limit, radius);
    return InRangeOnly(self, far_limit, radius);
}

inline int __fastcall HookB(void* self, void* edx, float far_limit, float radius, float arg3) {
    if (!WantsOverride(self))
        return original_b(self, edx, far_limit, radius, arg3);
    return InRangeOnly(self, far_limit, radius);
}

// The base-class virtual: no cached-flag write (this+0x1E means something
// else in the inheriting classes) and no special paths.
inline int __fastcall HookC(void* self, void* edx, float far_limit, float radius) {
    if (!vrmod::config.object_vis_fix || !gamecam::DrivesView())
        return original_c(self, edx, far_limit, radius);
    const auto* pos =
        reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(self) + OBJ_POS_OFFSET);
    return InRange(pos, far_limit, radius);
}

// The raw-position entry (the mag's path): plain cdecl, pure.
inline int __cdecl HookD(const float* pos, float far_limit, float radius) {
    const bool drives = vrmod::config.object_vis_fix && gamecam::DrivesView();
    const int ours = drives ? InRange(pos, far_limit, radius) : -1;
    const int result = drives ? ours : original_d(pos, far_limit, radius);
    return result;
}

inline float F(const void* base, uintptr_t off) {
    return *reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(base) + off);
}
inline uintptr_t P(const void* base, uintptr_t off) {
    return *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(base) + off);
}


// Mag one-tick pop fix ([vr] mag_glitch_fix): for
// one tick the bone matrices and the global root matrix (0x4D455C ->
// [0xA48980]) can disagree, jumping the target. If the mag stepped more
// than MAG_GLITCH_STEP while its owner's anchor (owner+0x300) moved less
// than MAG_GLITCH_ANCHOR, the snap flag (+0x64 bit 2) is clear, and the
// previous tick was calm (< MAG_GLITCH_PREV) and not itself held, the
// pre-update position is restored for that tick. The calm-tick rule stops
// it pinning a mag that is catching up after a spawn. Our own mag only:
// other mags' owners move by packets or AI and trip the rule.
constexpr float MAG_GLITCH_STEP = 1.2f;    // units; normal idle steps are ~0.01-0.05
constexpr float MAG_GLITCH_PREV = 0.4f;    // units; the tick before a glitch is calm
constexpr float MAG_GLITCH_ANCHOR = 0.5f;  // units; a warp moves more
inline int mag_fix_log_budget = 20;
inline float mag_prev_step = 0.0f;
inline bool mag_held_last = false;

// Mag target root sync ([vr] mag_root_sync). The
// hover target is the owner's bone matrix times the global root matrix at
// 0xA48980, which is the camera (world-from-camera). Bones are posed
// camera-relative during the render, so the update multiplies last
// frame's bones by this tick's camera and head turns swing every mag. The
// root is captured at the end of each rendered frame and swapped in
// around every mag update.
constexpr uintptr_t kGlobalRootMatrix = 0x00A48980;  // D3DMATRIX (weapongrip::GLOBAL_ROOT_MATRIX)
inline D3DMATRIX posed_root;
inline bool posed_root_valid = false;
inline unsigned root_synced_updates = 0;

inline void OnFrameEnd() {
    if (!diag::Accessible(kGlobalRootMatrix, 64, false)) {
        posed_root_valid = false;
        return;
    }
    memcpy(&posed_root, reinterpret_cast<const void*>(kGlobalRootMatrix), 64);
    posed_root_valid = true;
    diag::mag_posed_root_valid = true;
}

inline void RunMagUpdate(void* self, void* edx) {
    if (!vrmod::config.mag_root_sync || !posed_root_valid || !gamecam::DrivesView() ||
        !diag::Accessible(kGlobalRootMatrix, 64, true)) {
        original_mag_update(self, edx);
        return;
    }
    D3DMATRIX live;
    memcpy(&live, reinterpret_cast<const void*>(kGlobalRootMatrix), 64);
    memcpy(reinterpret_cast<void*>(kGlobalRootMatrix), &posed_root, 64);
    original_mag_update(self, edx);
    memcpy(reinterpret_cast<void*>(kGlobalRootMatrix), &live, 64);
    root_synced_updates++;
    diag::mag_root_synced = root_synced_updates;
}

inline void __fastcall HookMagUpdate(void* self, void* edx) {
    constexpr bool tracing = false;
    if (!diag::Accessible(reinterpret_cast<uintptr_t>(self), 0x100, false)) {
        RunMagUpdate(self, edx);
        return;
    }
    const uintptr_t owner0 = P(self, 0x20);
    const bool fixing = vrmod::config.mag_glitch_fix && gamecam::DrivesView() &&
                        owner0 != 0 && owner0 == gamecam::ResolveEntity();
    if (!tracing && !fixing) {
        RunMagUpdate(self, edx);
        return;
    }
    const float bx = F(self, 0xB4), by = F(self, 0xB8), bz = F(self, 0xBC);
    float anchor_before[3] = {};
    const bool have_anchor = owner0 && diag::Accessible(owner0 + 0x300, 12, false);
    if (have_anchor) {
        anchor_before[0] = F(reinterpret_cast<void*>(owner0), 0x300);
        anchor_before[1] = F(reinterpret_cast<void*>(owner0), 0x304);
        anchor_before[2] = F(reinterpret_cast<void*>(owner0), 0x308);
    }
    RunMagUpdate(self, edx);
    float ax = F(self, 0xB4), ay = F(self, 0xB8), az = F(self, 0xBC);
    if (fixing && have_anchor && (P(self, 0x64) & 2) == 0) {
        const float sx = ax - bx, sy = ay - by, sz = az - bz;
        const float step = sqrtf(sx * sx + sy * sy + sz * sz);
        const float ex = F(reinterpret_cast<void*>(owner0), 0x300) - anchor_before[0];
        const float ey = F(reinterpret_cast<void*>(owner0), 0x304) - anchor_before[1];
        const float ez = F(reinterpret_cast<void*>(owner0), 0x308) - anchor_before[2];
        const float anchor_move = sqrtf(ex * ex + ey * ey + ez * ez);
        const bool hold = step > MAG_GLITCH_STEP && anchor_move < MAG_GLITCH_ANCHOR &&
                          mag_prev_step < MAG_GLITCH_PREV && !mag_held_last;
        mag_prev_step = step;
        mag_held_last = hold;
        if (hold) {
            *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(self) + 0xB4) = bx;
            *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(self) + 0xB8) = by;
            *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(self) + 0xBC) = bz;
            ax = bx; ay = by; az = bz;
            if (mag_fix_log_budget > 0 || tracing) {
                if (mag_fix_log_budget > 0)
                    mag_fix_log_budget--;
                diag::Log("magfix: held the mag for one tick (step %.2f, anchor moved %.2f)",
                          step, anchor_move);
            }
        }
    }
}

// Called from the first BeginScene, next to textsnap::Install.
inline void Install() {
    static bool tried = false;
    if (tried)
        return;
    tried = true;

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        diag::Log("objvis: MH_Initialize failed (%d)", (int)status);
        return;
    }
    if (MH_CreateHook(reinterpret_cast<void*>(ONSCREEN_TEST_A),
                      reinterpret_cast<void*>(&HookA),
                      reinterpret_cast<void**>(&original_a)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(ONSCREEN_TEST_B),
                      reinterpret_cast<void*>(&HookB),
                      reinterpret_cast<void**>(&original_b)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(ONSCREEN_TEST_C),
                      reinterpret_cast<void*>(&HookC),
                      reinterpret_cast<void**>(&original_c)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(ONSCREEN_TEST_D),
                      reinterpret_cast<void*>(&HookD),
                      reinterpret_cast<void**>(&original_d)) != MH_OK ||
        MH_CreateHook(reinterpret_cast<void*>(MAG_UPDATE),
                      reinterpret_cast<void*>(&HookMagUpdate),
                      reinterpret_cast<void**>(&original_mag_update)) != MH_OK) {
        diag::Log("objvis: MH_CreateHook failed");
        return;
    }
    if (MH_EnableHook(reinterpret_cast<void*>(ONSCREEN_TEST_A)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(ONSCREEN_TEST_B)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(ONSCREEN_TEST_C)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(ONSCREEN_TEST_D)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(MAG_UPDATE)) != MH_OK) {
        diag::Log("objvis: MH_EnableHook failed");
        return;
    }
    probe::Log("objvis: on-screen test hooks installed at 0x%08X / 0x%08X / 0x%08X / 0x%08X",
               (unsigned)ONSCREEN_TEST_A, (unsigned)ONSCREEN_TEST_B,
               (unsigned)ONSCREEN_TEST_C, (unsigned)ONSCREEN_TEST_D);
}

}  // namespace objvis
