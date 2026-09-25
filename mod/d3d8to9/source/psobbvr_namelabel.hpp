// Player name labels - the name / level text floated over other
// characters.
//
// The label object: world anchor vec3 at +0x1C (the owner's head point),
// player id at +0x34, color at +0x3C, UTF-16 string at +0x40, state at
// +0x80 (6 = hidden). Its draw method 0x0078A1F8 (thiscall, no stack
// args, EAX passed through) calls the projector 0x00789A24
// (thiscall(this, float out[3]) -> on-screen; camera [0xA48A54] view
// fields +0x190..+0x198, a 350-unit distance cap, and finally
// x += [this+0x38] * [0x97A80C] = width * -0.5, i.e. it returns the
// string's start x). The string goes to the drawer 0x007AB554, +16 on x
// when the icon slot is in use ([0x9FD1AC] nonzero), and reaches the
// device through the glyph drawer 0x0082B440 (returns to 0x82B54F), one
// RHW 4-vertex fan per character.
//
// The hooks bracket the draw method and capture the anchor's world
// position, so the device routes the glyphs through the combat-text
// per-eye placement (stereo::IsCombatText / ProjectCombatTextVertices)
// at the anchor's depth. Passes through outside VR. [vr] name_labels.
#pragma once

#include <cstdint>
#include <cstring>
#include "MinHook.h"
#include "psobbvr_log.hpp"
#include "psobbvr_probe.hpp"

namespace namelabel {

constexpr uintptr_t LABEL_DRAW_ADDR = 0x0078A1F8;     // label draw method (thiscall, no args)
constexpr uintptr_t LABEL_PROJECT_ADDR = 0x00789A24;  // (this, float out[3]) -> on-screen, ret 4
constexpr size_t LABEL_ANCHOR_OFF = 0x1C;             // vec3 world anchor
constexpr size_t LABEL_WIDTH_OFF = 0x38;              // float: string width (flat px)
constexpr size_t LABEL_TEXT_OFF = 0x40;               // UTF-16 string
constexpr uintptr_t LABEL_CENTRE_K_ADDR = 0x0097A80C;  // float -0.5: the centring factor
constexpr uintptr_t LABEL_ICON_SLOT_ADDR = 0x009FD1AC;  // dword: nonzero = the draw method shifts the string +16 (icon slot)

using DrawFn = uint32_t(__fastcall*)(void* self, void* edx);
using ProjectFn = uint32_t(__fastcall*)(void* self, void* edx, float* out);
inline DrawFn original_draw = nullptr;
inline ProjectFn original_project = nullptr;

// The label whose draw method is executing (null outside), and what its
// projector produced this call.
inline void* bracket_obj = nullptr;
inline bool anchor_valid = false;
inline float anchor_world[3] = {};   // from the object (+0x1C)
// Per bracket: glyph draws routed so far and the first one's top-left.
// Placement is anchored on the first glyph, not the projector's point
// (under a menu the two are in different pixel spaces): the head's flat
// point is first.x + width/2 - (icon slot ? 16 : 0), first.y.
inline unsigned glyphs_in_bracket = 0;
inline float first_glyph[2] = {};
inline float width = 0.0f;           // [this+0x38], flat px
inline bool icon_slot = false;       // [0x9FD1AC] nonzero at the draw

inline bool Active() {
    return bracket_obj != nullptr && anchor_valid;
}

inline uint32_t __fastcall ProjectHook(void* self, void* edx, float* out) {
    const uint32_t r = original_project(self, edx, out);
    if (self == bracket_obj) {
        anchor_valid = r != 0 && out != nullptr;
        if (anchor_valid) {
            memcpy(anchor_world, static_cast<const uint8_t*>(self) + LABEL_ANCHOR_OFF, 12);
        }
    }
    return r;
}

inline uint32_t __fastcall DrawHook(void* self, void* edx) {
    void* const saved_obj = bracket_obj;
    const bool saved_valid = anchor_valid;
    bracket_obj = self;
    anchor_valid = false;
    glyphs_in_bracket = 0;
    width = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(self) + LABEL_WIDTH_OFF);
    icon_slot = *reinterpret_cast<const uint32_t*>(LABEL_ICON_SLOT_ADDR) != 0;
    const uint32_t r = original_draw(self, edx);
    bracket_obj = saved_obj;
    anchor_valid = saved_valid;
    return r;
}

inline void Install() {
    static bool tried = false;
    if (tried)
        return;
    tried = true;
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        diag::Log("namelabel: MH_Initialize failed (%d)", (int)status);
        return;
    }
    status = MH_CreateHook(reinterpret_cast<void*>(LABEL_DRAW_ADDR),
                           reinterpret_cast<void*>(&DrawHook),
                           reinterpret_cast<void**>(&original_draw));
    if (status == MH_OK)
        status = MH_EnableHook(reinterpret_cast<void*>(LABEL_DRAW_ADDR));
    if (status != MH_OK) {
        diag::Log("namelabel: draw hook failed (%d) - labels stay on the sprite route", (int)status);
        original_draw = nullptr;
        return;
    }
    status = MH_CreateHook(reinterpret_cast<void*>(LABEL_PROJECT_ADDR),
                           reinterpret_cast<void*>(&ProjectHook),
                           reinterpret_cast<void**>(&original_project));
    if (status == MH_OK)
        status = MH_EnableHook(reinterpret_cast<void*>(LABEL_PROJECT_ADDR));
    if (status != MH_OK) {
        diag::Log("namelabel: projector hook failed (%d) - labels stay on the sprite route", (int)status);
        original_project = nullptr;
        MH_DisableHook(reinterpret_cast<void*>(LABEL_DRAW_ADDR));
        original_draw = nullptr;
        return;
    }
    probe::Log("namelabel: label draw + projector hooks installed (0x%08X / 0x%08X)",
               (unsigned)LABEL_DRAW_ADDR, (unsigned)LABEL_PROJECT_ADDR);
}

}  // namespace namelabel
