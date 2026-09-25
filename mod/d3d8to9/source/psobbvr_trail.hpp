// Bullet-trail ribbon, rebuilt from the bullet's real path.
//
// The photon bullet's trail is an effect object (vtable 0xAFEE00); +0x2C
// is the tracked world position. Its update (vtable +0x14, 0x507120)
// projects that point each tick (0x830AA0 / 0x82F09C), keeps the current
// and previous screen points at +0x1C/+0x20 and +0x24/+0x28, and appends
// children (0x30 bytes, vtable 0xAFEDF0, ctor 0x50728C; oldest first)
// holding one 2D cross-section each at +0x20/+0x24 and +0x28/+0x2C. The
// ribbon draw (vtable +0x18, 0x506E94) draws one additive quad per visible
// pair [A_i, B_i, B_i+1, A_i+1] through the flat drawer 0x82B5D8 at a
// fixed depth of -15 units (z = 1 - min(1, -1/depth)). So the trail has no
// 3D data and would hang on a plane 15 units out.
//
// The hook records the bullet's world position per tick, walks the
// children as the draw does to get the section count and half widths,
// and rebuilds each quad from the newest history points in 3D, with each
// corner at its own depth. The world route then unprojects the corners
// back onto the path. Colors, states and winding stay the game's.
//
// trail_cross adds a second quad on the arm perpendicular to the path and
// the first arm, so the ribbon is a plus in cross-section and never
// vanishes edge-on. The arms are fixed in the world (the first one
// horizontal). trail_cross_gain scales both copies' colors (their additive
// overlap doubles the core).
//
// [vr] trail_depth_fix, trail_cross, trail_cross_gain.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
#include <windows.h>
#include "MinHook.h"
#include "psobbvr_log.hpp"
#include "psobbvr_resolution.hpp"
#include "psobbvr_stereo.hpp"
#include "psobbvr_vr.hpp"

namespace trail {

// Ribbon draw (thiscall, no args): walks [this+0x18] children via +0x10.
constexpr uintptr_t RIBBON_DRAW = 0x00506E94;
// Return address of the drawer call inside it.
constexpr uintptr_t RIBBON_DRAW_SITE = 0x005070F1;
// Object visibility (thiscall, returns nonzero = visible): walks the +0x14
// parent chain, fails on flags bit 0.
constexpr uintptr_t OBJECT_VISIBLE = 0x008173B4;
// The flat quad drawer's 2D scale/offset (vertex = corner * scale + offset).
constexpr uintptr_t SCALE_X = 0x00ACC0E8;
constexpr uintptr_t SCALE_Y = 0x00ACC0EC;
constexpr uintptr_t OFFSET_X = 0x00ACC0F0;
constexpr uintptr_t OFFSET_Y = 0x00ACC0F4;
constexpr uintptr_t NODE_NEXT = 0x10;
constexpr uintptr_t NODE_FIRST_CHILD = 0x18;
constexpr uintptr_t HEAD_POINT = 0x1C;       // current projected point x,y
constexpr uintptr_t HEAD_POS = 0x2C;         // tracked world position
constexpr uintptr_t SECTION_CORNER_A = 0x20;
constexpr uintptr_t SECTION_CORNER_B = 0x28;
constexpr size_t HISTORY_MAX = 256;
constexpr DWORD HISTORY_EXPIRE_MS = 3000;

using DrawFn = void(__fastcall*)(void* self, void* edx);
using VisibleFn = int(__fastcall*)(void* self, void* edx);

inline DrawFn original_draw = nullptr;

struct Section {
    float half_width;   // the game's |A - B| / 2, raw screen units
    float dir[2];       // the game's A - B direction (winding reference)
};
struct QuadRef {
    int a, b;           // section indices (oldest = 0)
    Section sa, sb;
};
struct History {
    uintptr_t self = 0;
    DWORD last_ms = 0;
    std::vector<std::array<float, 3>> pts;  // oldest first
};
inline std::vector<History> histories;
inline std::vector<QuadRef> quads;
inline size_t section_count = 0;
inline size_t cur_history = 0;
inline size_t next_quad = 0;
inline bool pending = false;
inline std::vector<uint8_t> scratch;
inline std::vector<uint8_t> companion;
inline bool companion_ready = false;

inline Section ReadSection(uintptr_t node) {
    Section s{};
    const float* a = reinterpret_cast<const float*>(node + SECTION_CORNER_A);
    const float* b = reinterpret_cast<const float*>(node + SECTION_CORNER_B);
    const float dx = a[0] - b[0], dy = a[1] - b[1];
    const float len = sqrtf(dx * dx + dy * dy);
    s.half_width = len * 0.5f;
    s.dir[0] = len > 1e-6f ? dx / len : 0.0f;
    s.dir[1] = len > 1e-6f ? dy / len : 1.0f;
    return s;
}

struct V3 { float x, y, z; };  // scene view space; z negative in front

inline V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 Add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 Mul(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 Cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Len(V3 a) { return sqrtf(Dot(a, a)); }
inline bool Normalize(V3& a) {
    const float l = Len(a);
    if (l < 1e-5f)
        return false;
    a = Mul(a, 1.0f / l);
    return true;
}

inline V3 ToView(const float* p) {
    const D3DMATRIX& v = stereo::cpu_view;
    return {p[0] * v._11 + p[1] * v._21 + p[2] * v._31 + v._41,
            p[0] * v._12 + p[1] * v._22 + p[2] * v._32 + v._42,
            p[0] * v._13 + p[1] * v._23 + p[2] * v._33 + v._43};
}

// World up in view space (rotation only).
inline V3 ViewUp() {
    const D3DMATRIX& v = stereo::cpu_view;
    return {v._21, v._22, v._23};
}

// View point -> the game's raw screen coordinates (the exact inverse of
// the world route's unprojection). False = at or behind the camera plane.
inline bool ViewToScreen(const V3& pt, float& sx, float& sy) {
    if (pt.z > -1e-3f)
        return false;
    const D3DMATRIX& pr = stereo::last_persp_proj;
    if (pr._11 == 0.0f || pr._22 == 0.0f)
        return false;
    const float w = -pt.z;
    const float ndc_x = pt.x * pr._11 / w - pr._31;
    const float ndc_y = pt.y * pr._22 / w - pr._32;
    const D3DVIEWPORT9& gvp = resolution::last_game_viewport;
    const float half_w = gvp.Width * 0.5f, half_h = gvp.Height * 0.5f;
    sx = gvp.X + half_w + ndc_x * half_w;
    sy = gvp.Y + half_h - ndc_y * half_h;
    return true;
}

inline bool Project(const float* p, float& sx, float& sy, float& zview) {
    const V3 pt = ToView(p);
    zview = pt.z;
    return ViewToScreen(pt, sx, sy);
}

// Screen half width (raw px) at a view depth -> view-space half width.
inline float ViewHalfWidth(float half_width_px, float zview) {
    const float scale = stereo::last_persp_proj._22 * resolution::last_game_viewport.Height * 0.5f;
    return scale > 1e-6f ? half_width_px * (-zview) / scale : 0.0f;
}

// The drawer's z for a view depth (see the header).
inline float DrawerZ(float zview) {
    float t = -1.0f / zview;
    if (t > 1.0f) t = 1.0f;
    return 1.0f - t;
}

inline size_t RecordHistory(uintptr_t self, DWORD now) {
    // Drop heads not drawn for a while (the bullet died), then find ours.
    for (size_t i = 0; i < histories.size();) {
        if (now - histories[i].last_ms > HISTORY_EXPIRE_MS)
            histories.erase(histories.begin() + i);
        else
            i++;
    }
    size_t idx = histories.size();
    for (size_t i = 0; i < histories.size(); i++)
        if (histories[i].self == self) { idx = i; break; }
    if (idx == histories.size()) {
        History h;
        h.self = self;
        histories.push_back(h);
    }
    History& h = histories[idx];
    h.last_ms = now;
    const float* p = reinterpret_cast<const float*>(self + HEAD_POS);
    std::array<float, 3> pos = {p[0], p[1], p[2]};
    bool moved = h.pts.empty();
    if (!moved) {
        const auto& l = h.pts.back();
        const float dx = pos[0] - l[0], dy = pos[1] - l[1], dz = pos[2] - l[2];
        moved = dx * dx + dy * dy + dz * dz > 1e-6f;
    }
    if (moved) {
        if (h.pts.size() >= HISTORY_MAX)
            h.pts.erase(h.pts.begin());
        h.pts.push_back(pos);
    }
    return idx;
}

inline void __fastcall HookDraw(void* self, void* edx) {
    quads.clear();
    next_quad = 0;
    section_count = 0;
    companion_ready = false;
    diag::trail_hook_calls++;
    const bool fixing = vrmod::config.trail_depth_fix && stereo::EffectReprojActive() &&
                        stereo::last_persp_proj_valid && self != nullptr;
    if (fixing) {
        const uintptr_t head = reinterpret_cast<uintptr_t>(self);
        cur_history = RecordHistory(head, GetTickCount());
        const auto visible = reinterpret_cast<VisibleFn>(OBJECT_VISIBLE);
        uintptr_t node = *reinterpret_cast<uintptr_t*>(head + NODE_FIRST_CHILD);
        int i = 0;
        while (node != 0 && i < 4096) {
            const uintptr_t next = *reinterpret_cast<uintptr_t*>(node + NODE_NEXT);
            if (next == 0)
                break;
            if (visible(reinterpret_cast<void*>(node), nullptr) &&
                visible(reinterpret_cast<void*>(next), nullptr))
                quads.push_back(QuadRef{i, i + 1, ReadSection(node), ReadSection(next)});
            node = next;
            i++;
        }
        section_count = (size_t)i + 1;  // the last child has no successor
        pending = !quads.empty();
    }
    diag::trail_last_quads = (unsigned)quads.size();
    original_draw(self, edx);
    pending = false;
    companion_ready = false;
}

// The cross quad built for the last overridden ribbon quad, once; null
// when there is none. The draw handler issues it right after the game's.
inline const void* TakeCompanion() {
    if (!companion_ready)
        return nullptr;
    companion_ready = false;
    return companion.data();
}

// Signed screen area of a 4-vertex fan (winding sign), from raw floats
// at the given stride.
inline float WindingSign(const uint8_t* verts, UINT stride) {
    float area = 0.0f;
    for (UINT i = 0; i < 4; i++) {
        const float* a = reinterpret_cast<const float*>(verts + size_t(i) * stride);
        const float* b = reinterpret_cast<const float*>(verts + size_t((i + 1) % 4) * stride);
        area += a[0] * b[1] - b[0] * a[1];
    }
    return area;
}

// Write a ribbon quad - points a/b, arm direction, half widths in view
// units - into buf (colors and rhw already there), projected per corner.
// Winding is flipped to match the game's sign. False = a corner at or
// behind the camera plane.
inline bool BuildQuad(uint8_t* buf, UINT stride, V3 a, V3 b, V3 arm, float la, float lb,
                      float sx, float sy, float ox, float oy, float game_sign) {
    const V3 c[4] = {Add(a, Mul(arm, la)), Sub(a, Mul(arm, la)),
                     Sub(b, Mul(arm, lb)), Add(b, Mul(arm, lb))};
    float px[4], py[4];
    for (UINT i = 0; i < 4; i++)
        if (!ViewToScreen(c[i], px[i], py[i]))
            return false;
    float area = 0.0f;
    for (UINT i = 0; i < 4; i++)
        area += px[i] * py[(i + 1) % 4] - px[(i + 1) % 4] * py[i];
    // Fan order 0,1,2,3 -> 0,3,2,1 reverses the winding.
    const bool flip = (area < 0.0f) != (game_sign < 0.0f);
    static const UINT order_keep[4] = {0, 1, 2, 3}, order_flip[4] = {0, 3, 2, 1};
    const UINT* order = flip ? order_flip : order_keep;
    for (UINT i = 0; i < 4; i++) {
        float* v = reinterpret_cast<float*>(buf + size_t(i) * stride);
        const UINT k = order[i];
        v[0] = px[k] * sx + ox;
        v[1] = py[k] * sy + oy;
        v[2] = DrawerZ(c[k].z);
    }
    return true;
}

inline void ScaleColors(uint8_t* buf, UINT stride, float g) {
    if (g < 0.0f) g = 0.0f;
    if (g > 1.0f) g = 1.0f;
    for (UINT i = 0; i < 4; i++) {
        uint32_t& c = *reinterpret_cast<uint32_t*>(buf + size_t(i) * stride + 16);
        const uint32_t r = (uint32_t)(((c >> 16) & 0xFF) * g), gg = (uint32_t)(((c >> 8) & 0xFF) * g),
                       bl = (uint32_t)((c & 0xFF) * g);
        c = (c & 0xFF000000u) | (r << 16) | (gg << 8) | bl;
    }
}

// DrawPrimitiveUP: for the ribbon drawer's quads (while the hook is on the
// stack), return a copy with all four vertices re-placed from the bullet's
// recorded path; otherwise the data unchanged. Stride 20 = XYZRHW|DIFFUSE.
inline const void* OverrideDepths(const void* data, UINT vertex_count, UINT stride,
                                  uintptr_t site) {
    if (!pending || site != RIBBON_DRAW_SITE || data == nullptr || vertex_count != 4 ||
        stride < 20 || next_quad >= quads.size() || cur_history >= histories.size())
        return data;
    const QuadRef q = quads[next_quad++];
    const History& h = histories[cur_history];
    scratch.assign(static_cast<const uint8_t*>(data),
                   static_cast<const uint8_t*>(data) + size_t(vertex_count) * stride);
    auto collapse = [&]() {
        float* v0 = reinterpret_cast<float*>(scratch.data());
        for (UINT i = 1; i < 4; i++) {
            float* v = reinterpret_cast<float*>(scratch.data() + size_t(i) * stride);
            v[0] = v0[0];
            v[1] = v0[1];
        }
        return scratch.data();
    };
    // Sections map onto the newest points of the history, oldest first.
    const long base = (long)h.pts.size() - (long)section_count;
    const long ia = base + q.a, ib = base + q.b;
    if (ia < 0 || ib < 0 || ia >= (long)h.pts.size() || ib >= (long)h.pts.size())
        return collapse();  // history shorter than the ribbon
    const V3 va = ToView(h.pts[(size_t)ia].data());
    const V3 vb = ToView(h.pts[(size_t)ib].data());
    if (va.z > -1e-3f || vb.z > -1e-3f)
        return collapse();  // at or behind the camera plane
    // Arms. d = the path in view space; n = the first arm, perpendicular
    // to it; b = the second, perpendicular to both.
    V3 d = Sub(vb, va);
    if (!Normalize(d))
        d = V3{0.0f, 0.0f, -1.0f};
    V3 n = Cross(d, ViewUp());  // horizontal arm, world-fixed
    bool have_n = Normalize(n);
    if (!have_n) {
        // Vertical path: an arm in the screen plane (the vanilla look).
        n = V3{-d.y, d.x, 0.0f};
        have_n = Normalize(n);
    }
    if (!have_n) {
        // Path straight down the view axis: use the game's own arm.
        n = V3{q.sa.dir[0], -q.sa.dir[1], 0.0f};
        if (!Normalize(n))
            n = V3{1.0f, 0.0f, 0.0f};
    }
    // Keep the game's side for A (its corners are A = +arm, B = -arm).
    if (n.x * q.sa.dir[0] - n.y * q.sa.dir[1] < 0.0f)
        n = Mul(n, -1.0f);
    V3 b2 = Cross(d, n);
    if (!Normalize(b2))
        b2 = V3{0.0f, 0.0f, 1.0f};
    const float la = ViewHalfWidth(q.sa.half_width, va.z);
    const float lb = ViewHalfWidth(q.sb.half_width, vb.z);
    float sx = 1.0f, sy = 1.0f, ox = 0.0f, oy = 0.0f;
    if (diag::Accessible(SCALE_X, 16, false)) {
        sx = *reinterpret_cast<const float*>(SCALE_X); sy = *reinterpret_cast<const float*>(SCALE_Y);
        ox = *reinterpret_cast<const float*>(OFFSET_X); oy = *reinterpret_cast<const float*>(OFFSET_Y);
    }
    const float game_sign = WindingSign(static_cast<const uint8_t*>(data), stride);
    if (!BuildQuad(scratch.data(), stride, va, vb, n, la, lb, sx, sy, ox, oy, game_sign))
        return collapse();
    if (vrmod::config.trail_cross) {
        ScaleColors(scratch.data(), stride, vrmod::config.trail_cross_gain);
        companion = scratch;
        companion_ready = BuildQuad(companion.data(), stride, va, vb, b2, la, lb, sx, sy, ox, oy,
                                    game_sign);
    }
    return scratch.data();
}

// Called from the first BeginScene, next to objvis::Install.
inline void Install() {
    static bool tried = false;
    if (tried)
        return;
    tried = true;
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        diag::trail_install_status = (int)status;
        diag::Log("trail: MH_Initialize failed (%d)", (int)status);
        return;
    }
    status = MH_CreateHook(reinterpret_cast<void*>(RIBBON_DRAW), reinterpret_cast<void*>(&HookDraw),
                           reinterpret_cast<void**>(&original_draw));
    if (status == MH_OK)
        status = MH_EnableHook(reinterpret_cast<void*>(RIBBON_DRAW));
    diag::trail_install_status = (int)status;
    if (status != MH_OK) {
        diag::Log("trail: hook install failed (%d)", (int)status);
        return;
    }
    diag::trail_installed = true;
    diag::Log("trail: ribbon draw hook installed at 0x%08X", (unsigned)RIBBON_DRAW);
}

}  // namespace trail
