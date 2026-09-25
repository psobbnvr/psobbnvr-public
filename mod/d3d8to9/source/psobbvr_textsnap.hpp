// Combat-text capture (placement is in psobbvr_stereo.hpp).
//
// Every damage number / MISS / RESIST / EXP popup is a 0x68-byte object
// (vtable 0x00B450F0) that keeps the hit's world position at
// +0x1C/+0x20/+0x24 for its whole life. Constructors:
//   0x0078A5E8 (pos*, colorRGB, lifetime, number)          damage numbers
//   0x0078A468 (pos*, colorRGB, lifetime, wstr*)           MISS (L"MISS" @
//                0x00930848, red), RESIST, other fixed strings
//   0x0078A944 (pos*, colorRGB, lifetime, delay, wstr*, number)  e.g. EXP
//   0x0078A7F8 (this, pos*, colorRGB, lifetime, delay, wstr*, slot)
// Fed by 0x00777414 (subtracts enemy HP +0x334) -> 0x0077535C ->
// 0x0078A5E8; the MISS wrapper is 0x00777450.
//
// The draw virtual (0x0078ABD0) re-projects the position every frame: it
// rebuilds global matrices from the internal camera copy at [0x00A48A54]
// (which lags the VR head pose) and projects with the linked
// D3DXVec3Project (0x00890FC7) using:
//   projection matrix @ 0x00ACBF40      view matrix @ 0x00ACBF80
//   world matrix      @ 0x00ACC000      D3DVIEWPORT8 @ 0x00ACC0A8
// then subtracts the offset pair @ 0x00ACC0F0/0x00ACC0F4.
// Object fields (update 0x0078AB74, draw 0x0078ABD0):
//   +0x1C/20/24 world x,y,z      +0x28 screen-x jitter (random, at spawn)
//   +0x2C screen-y float-up      +0x30 string half-width
//   +0x34 wchar_t string[15]     +0x54 color RGB
//   +0x58 age (frames)           +0x5C lifetime (damage passes 0x14 = 20)
//   +0x60 pre-display delay      +0x64 active flag (word)
//   +0x66 player-slot filter (word, 0xFFFF = everyone)
//
// Each character is one immediate 4-vertex fan DrawPrimitiveUP through the
// game's shared glyph drawer 0x0082B440 (device call returns to
// 0x0082B54F; all game text uses it). 0x007893A4 fills the glyph UVs,
// 0x00789044 gives the advance width. The string starts at the projected
// point + jitter (always leftward) and runs right.
//
// The hook snapshots each active object, then brackets the original draw
// with current_snap, so the device hooks know exactly which object issued
// each glyph. psobbvr_stereo.hpp unprojects those draws through the same
// text camera (RefreshCam) at the object's depth, so the camera lag
// cancels.
#pragma once

#include <cstdint>
#include <cwchar>
#include "MinHook.h"
#include "psobbvr_probe.hpp"
#include "psobbvr_log.hpp"

namespace textsnap {

constexpr uintptr_t TEXT_DRAW_ADDR = 0x0078ABD0;  // floating-text draw vtbl[2]

// The game's 2D/text projection context globals (see header comment).
constexpr uintptr_t TEXT_CTX_PROJ = 0x00ACBF40;      // D3DMATRIX
constexpr uintptr_t TEXT_CTX_VIEW = 0x00ACBF80;      // D3DMATRIX
constexpr uintptr_t TEXT_CTX_WORLD = 0x00ACC000;     // D3DMATRIX
constexpr uintptr_t TEXT_CTX_VIEWPORT = 0x00ACC0A8;  // D3DVIEWPORT8
constexpr uintptr_t TEXT_CTX_OFF_X = 0x00ACC0F0;     // float, subtracted
constexpr uintptr_t TEXT_CTX_OFF_Y = 0x00ACC0F4;     //   post-projection

struct Snap {
    const void* obj;      // game object pointer (pool memory - can be reused)
    float world[3];
    float jitter_x;       // +0x28, constant per burst
    float float_y;        // +0x2C, the float-up, grows per frame
    int age;              // +0x58 at last sight
    int lifetime;         // +0x5C
    unsigned color;       // +0x54
    wchar_t text[16];     // +0x34, for logging
    int last_seen_frame;  // our frame counter (see OnFrameEnd)
};

inline Snap snaps[64];
inline int snap_count = 0;
inline int frame = 0;  // ticked by stereo::OnFrameEnd

// The snapshot of the object being drawn while inside the original draw;
// null outside. The device hooks classify combat text by it.
inline const Snap* current_snap = nullptr;

// The game's 2D/text projection context, pulled live by RefreshCam.
struct FrameCam {
    float world[16], view[16], proj[16];
    float vp_x = 0, vp_y = 0, vp_w = 0, vp_h = 0;
    float off_x = 0, off_y = 0;
    int frame = -1;  // textsnap::frame at capture; -1 = never captured
};
inline FrameCam cam;

using DrawFn = void(__fastcall*)(void* obj, void* edx);
inline DrawFn original_draw = nullptr;

// __fastcall stands in for __thiscall: the object arrives in ecx, edx is
// a dummy.
inline void __fastcall DrawHook(void* obj, void* edx) {
    const uint8_t* o = static_cast<const uint8_t*>(obj);
    const bool active = *reinterpret_cast<const int16_t*>(o + 0x64) != 0;
    if (active) {
        const int age = *reinterpret_cast<const int*>(o + 0x58);
        Snap* s = nullptr;
        for (int i = 0; i < snap_count; i++) {
            if (snaps[i].obj == obj) {
                s = &snaps[i];
                break;
            }
        }
        bool fresh = false;
        if (s == nullptr && snap_count < 64) {
            s = &snaps[snap_count++];
            s->obj = obj;
            fresh = true;
        }
        if (s != nullptr) {
            // Age going backwards = a new burst in a reused slot.
            if (!fresh && age < s->age)
                fresh = true;
            s->world[0] = *reinterpret_cast<const float*>(o + 0x1c);
            s->world[1] = *reinterpret_cast<const float*>(o + 0x20);
            s->world[2] = *reinterpret_cast<const float*>(o + 0x24);
            s->jitter_x = *reinterpret_cast<const float*>(o + 0x28);
            s->float_y = *reinterpret_cast<const float*>(o + 0x2c);
            s->age = age;
            s->lifetime = *reinterpret_cast<const int*>(o + 0x5c);
            s->color = *reinterpret_cast<const unsigned*>(o + 0x54);
            wcsncpy_s(s->text, reinterpret_cast<const wchar_t*>(o + 0x34), 15);
            s->last_seen_frame = frame;
            current_snap = s;
        }
    }
    // Glyph draws issued inside arrive with current_snap set.
    original_draw(obj, edx);
    current_snap = nullptr;
}

// Copies the text-projection globals into 'cam', once per frame, when the
// first bracketed glyph arrives: the draw rebuilds them before projecting
// (0x0078AC1F..0x0078AC6E), so they match this burst's projection.
inline void RefreshCam() {
    if (cam.frame == frame)
        return;
    memcpy(cam.proj, reinterpret_cast<const void*>(TEXT_CTX_PROJ), 64);
    memcpy(cam.view, reinterpret_cast<const void*>(TEXT_CTX_VIEW), 64);
    memcpy(cam.world, reinterpret_cast<const void*>(TEXT_CTX_WORLD), 64);
    const uint32_t* vp = reinterpret_cast<const uint32_t*>(TEXT_CTX_VIEWPORT);
    cam.vp_x = (float)vp[0];
    cam.vp_y = (float)vp[1];
    cam.vp_w = (float)vp[2];
    cam.vp_h = (float)vp[3];
    cam.off_x = *reinterpret_cast<const float*>(TEXT_CTX_OFF_X);
    cam.off_y = *reinterpret_cast<const float*>(TEXT_CTX_OFF_Y);
    cam.frame = frame;
    static bool logged_once = false;
    if (!logged_once) {
        logged_once = true;
        diag::Log("textsnap: cam proj_diag=(%.4f,%.4f,%.4f,%.4f) vp=(%.0f,%.0f,%.0f,%.0f) off=(%.1f,%.1f)",
                  cam.proj[0], cam.proj[5], cam.proj[10], cam.proj[14],
                  cam.vp_x, cam.vp_y, cam.vp_w, cam.vp_h,
                  cam.off_x, cam.off_y);
    }
}

// Called from stereo::OnFrameEnd: advances the frame counter and drops
// snapshots whose object stopped drawing.
inline void OnFrameEnd() {
    frame++;
    for (int i = 0; i < snap_count;) {
        if (snaps[i].last_seen_frame < frame - 4)
            snaps[i] = snaps[--snap_count];
        else
            i++;
    }
}

// Called from the first BeginScene, next to texguard::Install.
inline void Install() {
    static bool tried = false;
    if (tried)
        return;
    tried = true;

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        diag::Log("textsnap: MH_Initialize failed (%d)", (int)status);
        return;
    }
    status = MH_CreateHook(reinterpret_cast<void*>(TEXT_DRAW_ADDR),
                           reinterpret_cast<void*>(&DrawHook),
                           reinterpret_cast<void**>(&original_draw));
    if (status != MH_OK) {
        diag::Log("textsnap: MH_CreateHook failed (%d)", (int)status);
        return;
    }
    status = MH_EnableHook(reinterpret_cast<void*>(TEXT_DRAW_ADDR));
    if (status != MH_OK) {
        diag::Log("textsnap: MH_EnableHook failed (%d)", (int)status);
        return;
    }
    probe::Log("textsnap: floating-text draw hook installed at 0x%08X",
               (unsigned)TEXT_DRAW_ADDR);
}

}  // namespace textsnap
