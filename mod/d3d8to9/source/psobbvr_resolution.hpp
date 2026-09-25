#pragma once

// Render-resolution override.
//
// The game hardcodes a 640x480 backbuffer. The backbuffer is created at
// the configured size and the game still sees 640x480: viewports and the
// screen-space (RHW) vertices of its UI (all DrawPrimitiveUP with XYZRHW)
// are scaled; 3D geometry scales through the projection.
//
// psobbvr.ini in the current directory (the game folder):
//   [render]
//   width=2880
//   height=2160
// Missing lines mean 2880x2160; 640x480 disables the override.

#include <cmath>
#include <cstdint>
#include <vector>
#include <windows.h>
#include <d3d9.h>

namespace resolution {

// The resolution the game believes it runs at.
constexpr float kGameWidth = 640.0f;
constexpr float kGameHeight = 480.0f;

inline bool enabled = false;
inline UINT target_width = 0;
inline UINT target_height = 0;
inline float scale_x = 1.0f;
inline float scale_y = 1.0f;

// While true, viewports and RHW vertices pass through unscaled. Set through
// the exported PsobbvrSetScalePassthrough by the dinput8 overlay, which
// draws in real backbuffer pixels.
inline bool passthrough = false;

// True when game-space -> backbuffer-space coordinate scaling should apply
// to the current call.
inline bool ScalingActive() { return enabled && !passthrough; }

// Last viewport the game set, unscaled, so GetViewport can keep the illusion.
inline D3DVIEWPORT9 last_game_viewport = { 0, 0, 640, 480, 0.0f, 1.0f };

inline std::vector<uint8_t> scratch;

inline void LoadConfig() {
    char path[MAX_PATH];
    if (!GetCurrentDirectoryA(MAX_PATH, path))
        return;
    strcat_s(path, "\\psobbvr.ini");
    const UINT w = GetPrivateProfileIntA("render", "width", 2880, path);
    const UINT h = GetPrivateProfileIntA("render", "height", 2160, path);
    if (w >= 640 && h >= 480 && w <= 16384 && h <= 16384 && !(w == 640 && h == 480)) {
        target_width = w;
        target_height = h;
        scale_x = w / kGameWidth;
        scale_y = h / kGameHeight;
        enabled = true;
    }
}

// Applies to both CreateDevice and Reset. Only engages when the parameters
// look like the game's own 640x480 request (explicit or window-derived).
inline void OverridePresentParameters(D3DPRESENT_PARAMETERS& params) {
    if (!enabled || !params.Windowed)
        return;
    const bool explicit_640 = params.BackBufferWidth == 640 && params.BackBufferHeight == 480;
    const bool window_derived = params.BackBufferWidth == 0 && params.BackBufferHeight == 0;
    if (explicit_640 || window_derived) {
        params.BackBufferWidth = target_width;
        params.BackBufferHeight = target_height;
    }
}

inline bool IsRhwFvf(DWORD fvf) {
    return (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
}

inline UINT VertexCountForPrimitives(D3DPRIMITIVETYPE type, UINT primitives) {
    switch (type) {
    case D3DPT_POINTLIST: return primitives;
    case D3DPT_LINELIST: return primitives * 2;
    case D3DPT_LINESTRIP: return primitives + 1;
    case D3DPT_TRIANGLELIST: return primitives * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN: return primitives + 2;
    default: return 0;
    }
}

// Copy the caller's screen-space vertices and scale x/y (first two floats)
// into backbuffer pixels. Returns the scaled copy, valid until the next call.
inline const void* ScaleRhwVertices(const void* data, UINT vertex_count, UINT stride) {
    const auto* src = static_cast<const uint8_t*>(data);
    scratch.assign(src, src + size_t(vertex_count) * stride);
    uint8_t* vertex = scratch.data();
    for (UINT i = 0; i < vertex_count; i++, vertex += stride) {
        float* xy = reinterpret_cast<float*>(vertex);
        xy[0] *= scale_x;
        xy[1] *= scale_y;
    }
    return scratch.data();
}

} // namespace resolution
