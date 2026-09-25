#pragma once

// D3D9Ex device support. VR hands the eye render targets to an in-process
// D3D11 device through shared handles (OpenVR's Submit rejects D3D9
// textures), and shared handles need a device created through the 9Ex
// interfaces - so Direct3DCreate8 goes through Direct3DCreate9Ex /
// CreateDeviceEx, with plain D3D9 as the automatic fallback.
//
// A D3D9Ex device rejects D3DPOOL_MANAGED (where nearly every D3D8 game
// keeps its resources; Ex devices never lose resources, so the pool was
// dropped). RemapPool() turns those creations into D3DPOOL_DEFAULT +
// D3DUSAGE_DYNAMIC, which keeps them lockable. A driver that rejects a
// format as DYNAMIC would show up as a logged creation failure; there is
// no system-memory staging fallback.

#include <windows.h>
#include <d3d9.h>

namespace d3d9ex {

// True when the running IDirect3D9 really is an IDirect3D9Ex and the device
// will be / was created with CreateDeviceEx.
inline bool active = false;

// PSOBBVR_NO_D3D9EX=1 forces the plain D3D9 path (for bisecting startup or
// rendering problems).
inline bool Disabled() {
    return GetEnvironmentVariableA("PSOBBVR_NO_D3D9EX", nullptr, 0) != 0;
}

// D3D9Ex rejects D3DPOOL_MANAGED; DEFAULT + DYNAMIC keeps the resource
// lockable. Called by every resource-creation wrapper before forwarding.
inline void RemapPool(D3DPOOL &pool, DWORD &usage) {
    if (!active || pool != D3DPOOL_MANAGED)
        return;
    pool = D3DPOOL_DEFAULT;
    usage |= D3DUSAGE_DYNAMIC;
}

} // namespace d3d9ex
