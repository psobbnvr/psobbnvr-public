// Null guard for the game's texture-bind helper: prevents a crash in the
// character select -> lobby load. (It may rarely fire since the D3D9Ex
// mip-chain overrun fix in d3d8to9_texture.cpp; kept as cheap protection.)
//
// The helper 0x8399D4 (texture record in ecx, no stack args) dereferences
// the record's inner pointer [ecx] ([inner+0] = cache key vs [0xACD540],
// [inner+8] = IDirect3DTexture8* for stage 0). The game keeps a scratch
// record at 0xACB3C0, refilled by a keyed lookup (0x829DA8: registry of
// 0x30-byte entries at [0xACB9C4], stores [entry+0x20], then binds). A
// second path (0x829D49) re-binds it without refilling. The load zeroes
// the block holding the record (0xACB3B0..0xACB430+), and a re-bind then
// reads NULL at 0x8399E7.
//
// The guard skips the bind when the record or its inner pointer is null.
// The helper has its own skip paths (flag bit 2 at [0xACD564], key -1 in
// the palette sibling 0x83997C), so this at worst leaves the previous
// texture bound for one loading-screen draw.
#pragma once

#include <cstdint>
#include "MinHook.h"
#include "psobbvr_probe.hpp"

namespace texguard {

constexpr uintptr_t BIND_HELPER_ADDR = 0x008399D4;

// __fastcall stands in for __thiscall: the record arrives in ecx, edx is a
// dummy the original ignores.
using BindFn = void(__fastcall*)(void* record, void* edx);
inline BindFn original_bind = nullptr;

inline void __fastcall BindHook(void* record, void* edx) {
    if (record == nullptr || *reinterpret_cast<void* const*>(record) == nullptr)
        return;
    original_bind(record, edx);
}

// Called from the first BeginScene (game fully initialized, render thread).
inline void Install() {
    static bool tried = false;
    if (tried)
        return;
    tried = true;

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        probe::Log("texguard: MH_Initialize failed (%d)", (int)status);
        return;
    }
    status = MH_CreateHook(reinterpret_cast<void*>(BIND_HELPER_ADDR),
                           reinterpret_cast<void*>(&BindHook),
                           reinterpret_cast<void**>(&original_bind));
    if (status != MH_OK) {
        probe::Log("texguard: MH_CreateHook failed (%d)", (int)status);
        return;
    }
    status = MH_EnableHook(reinterpret_cast<void*>(BIND_HELPER_ADDR));
    if (status != MH_OK) {
        probe::Log("texguard: MH_EnableHook failed (%d)", (int)status);
        return;
    }
    probe::Log("texguard: null-guard installed on texture-bind helper 0x%08X",
               (unsigned)BIND_HELPER_ADDR);
}

}  // namespace texguard
