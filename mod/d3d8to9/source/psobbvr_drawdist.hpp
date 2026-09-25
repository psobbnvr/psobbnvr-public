#pragma once

// Extends the level geometry draw distance ([vr] draw_distance_scale).
// Two parts:
//
//  1. Clip distance. Per-episode multipliers in .data (0x97F1C0 = 1.0f
//     Ep1/fallback, 0x97F1B8 = 1.5f Ep2, 0x97F1BC = 2.0f Ep4; episode word
//     0xA46C6A, getter 0x482A30) feed the area-setup builder 0x807448
//     (world-render context in EAX; cdecl pre-entry 0x807444), run at area
//     load from 0x8072E0 / 0x80B865: base distances at 0x97F1A8..B4
//     (0.5 / 1200 / 800 / 600) x multiplier -> a per-area table from the
//     template at 0x97EDE0 -> the clip distance in [[0xAAFCA4]+0xC]. The
//     multipliers are scaled and the builder re-run (it is idempotent) so
//     a change applies mid-area.
//
//  2. Section evaluator. Maps are split into sections (stride 0x34 at
//     [[0xAAFCA4]+0x10], count = word [[0xAAFCA4]+8]; bit 0x10000 in +0x30
//     = draw). Each frame the dispatch at 0x80616E (call [eax*4+0xA11478],
//     after clearing all bits at 0x80613F) picks an evaluator by the
//     selector 0x806184:
//       0 = 0x809098 generic: inside the section radius (+0x1C), or within
//           the clip distance and inside a 60-degree cone of the internal
//           camera's yaw or touching a half-FOV edge ray.
//       1 = 0x807F8C variant used for mode-10/11 areas 1-2.
//       2 = 0x8086A8 authored: when the map has a visibility list
//           ([0xAAFC4C] non-null, entries {section id, neighbor count,
//           neighbor offset} stride 0xC), only the camera's section and
//           its listed neighbors are drawn - the cause of the Forest
//           canopy pop-in.
//       3 = 0x8095CC distance only ([0xAAFC80] == 1 mode).
//       4 = 0x809694 mark everything (special areas 0x28/0x2C).
//     With scale > 1 the selector hook remaps 2 -> 0, so the clip distance
//     becomes the limit. More sections cost more per tick, and the cone
//     tests use the internal camera ([0xA48A54]), which lags the head, so
//     fast look-backs can show sections filling in.
//
// The projection far plane is 21000 units (e.g. 0x4D2AB8) - never a limit.

#include <cstdint>
#include <cstring>
#include <windows.h>

#include "MinHook.h"
#include "psobbvr_log.hpp"
#include "psobbvr_probe.hpp"
#include "psobbvr_vr.hpp"

namespace drawdist {

// Per-episode clip-distance multipliers and their stock values.
struct Multiplier { uintptr_t addr; float stock; };
constexpr Multiplier MULTIPLIERS[] = {
    { 0x0097F1C0, 1.0f },  // Ep1 + fallback
    { 0x0097F1B8, 1.5f },  // Ep2
    { 0x0097F1BC, 2.0f },  // Ep4
};

// The world-render context pointer the area-setup builder takes.
constexpr uintptr_t WORLD_CTX_PTR = 0x00AAFCA4;
// The builder's __cdecl stack-arg pre-entry (loads EAX from the stack and
// falls into the real entry at 0x807448; plain ret, caller cleans).
using AreaDistanceBuilderFn = void(__cdecl*)(void* world_ctx);
const auto AreaDistanceBuilder = reinterpret_cast<AreaDistanceBuilderFn>(0x00807444);

// The per-frame section-visibility evaluator selector (no args, index in
// eax; single caller 0x806167, feeding the dispatch at 0x80616E).
constexpr uintptr_t EVAL_SELECTOR_ADDR = 0x00806184;
constexpr int EVAL_GENERIC = 0;   // 0x809098 distance/cone tests
constexpr int EVAL_AUTHORED = 2;  // 0x8086A8 authored neighbor lists
using EvalSelectorFn = int(__cdecl*)();
inline EvalSelectorFn original_selector = nullptr;

// Scale currently written into the multipliers; 0 = stock values in place.
// Also gates the selector remap.
inline float applied_scale = 0.0f;

inline int __cdecl EvalSelectorHook() {
    const int idx = original_selector();
    // Only when extending: with a short clip the generic evaluator could
    // hide the section in view.
    if (applied_scale > 1.0f && idx == EVAL_AUTHORED)
        return EVAL_GENERIC;
    return idx;
}

// Called from the first OnFrame (game fully initialized, render thread).
inline void InstallSelectorHook() {
    static bool tried = false;
    if (tried)
        return;
    tried = true;

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        probe::Log("drawdist: MH_Initialize failed (%d)", (int)status);
        return;
    }
    status = MH_CreateHook(reinterpret_cast<void*>(EVAL_SELECTOR_ADDR),
                           reinterpret_cast<void*>(&EvalSelectorHook),
                           reinterpret_cast<void**>(&original_selector));
    if (status != MH_OK) {
        probe::Log("drawdist: MH_CreateHook failed (%d)", (int)status);
        return;
    }
    status = MH_EnableHook(reinterpret_cast<void*>(EVAL_SELECTOR_ADDR));
    if (status != MH_OK) {
        probe::Log("drawdist: MH_EnableHook failed (%d)", (int)status);
        return;
    }
    probe::Log("drawdist: evaluator-selector hook installed on 0x%08X",
               (unsigned)EVAL_SELECTOR_ADDR);
}

inline void WriteMultipliers(float scale) {
    for (const Multiplier& m : MULTIPLIERS) {
        const float value = scale == 0.0f ? m.stock : m.stock * scale;
        DWORD old_protect;
        if (VirtualProtect(reinterpret_cast<void*>(m.addr), 4, PAGE_READWRITE, &old_protect)) {
            memcpy(reinterpret_cast<void*>(m.addr), &value, 4);
            VirtualProtect(reinterpret_cast<void*>(m.addr), 4, old_protect, &old_protect);
        }
    }
}

// Called every frame from BeginScene; writes only on a change. Not gated
// on VR driving the view: the multipliers must be in place when an area
// load (a non-gameplay frame) runs the builder.
inline void OnFrame() {
    InstallSelectorHook();
    float want = 0.0f;
    if (vrmod::config.enabled) {
        const float s = vrmod::config.draw_distance_scale;
        if (s > 0.0f && s != 1.0f)
            want = s;
    }
    if (want == applied_scale)
        return;
    WriteMultipliers(want);
    applied_scale = want;
    // Re-run the area-distance setup so the change applies mid-area. The
    // context is null between areas; the load then picks the values up.
    const uintptr_t ctx = diag::Accessible(WORLD_CTX_PTR, 4, false)
        ? *reinterpret_cast<const uintptr_t*>(WORLD_CTX_PTR) : 0;
    if (ctx != 0 && diag::Accessible(ctx, 0x14, false))
        AreaDistanceBuilder(reinterpret_cast<void*>(ctx));
}

} // namespace drawdist
