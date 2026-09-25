#pragma once
#include <cstdint>

// Fixed absolute addresses in the Tethealla v12513 PSOBB client.
// Safe to hardcode: the exe has no ASLR, relocations are stripped, and the
// image base is always 0x00400000 (verified).

namespace psobb {

// --- From Solybum/psobbaddonplugin, bbmod/src/dinput8.cpp ---

// Game function that creates the D3D8 device. Args in ecx (small integer,
// seen as 2) and edx (seen as 0); returns a status in eax that the caller
// checks, so a hook must forward it or the game exits. Once it returns,
// the device pointer global below is set. Hooked to install the overlay.
constexpr uintptr_t kInitD3DFunc = 0x00838CA0;

// IDirect3DDevice8* — the game's own global holding its device. The
// developer build replaces this pointer with a wrapper object to intercept
// Present (data hook, no code patch).
constexpr uintptr_t kDevicePointer = 0x00ACD528;

// HWND — the game window.
constexpr uintptr_t kWindowHandle = 0x00ACBED8;


} // namespace psobb
