#pragma once

// Stereo rendering. Every game draw at the backbuffer is issued once per
// eye into the two halves of one double-wide render target, with per-eye
// view/projection (the headset's when VR runs, otherwise a plain
// separation offset on the game's view). At frame end the halves are
// composited side by side onto the backbuffer for the flat window, and the
// VR backend submits the same target. Also here: the HUD and menu quad
// textures and their routing, and the reprojection of the game's
// CPU-projected sprites and combat text.
//
// With stereo on at launch the backbuffer is created double wide, so each
// eye gets a full-resolution half (the dinput8 overlay sizes the window to
// match). Switched on live with a normal backbuffer, the halves are
// squeezed. Switched off live on a double-wide backbuffer, both halves
// render identically.
//
// The dinput8 overlay calls PsobbvrStereoFinishFrame() before drawing its
// ImGui panel, so the panel lands on top of the composite; without the
// overlay, Present() runs the composite.
//
// psobbvr.ini:
//   [stereo]
//   enabled=1        ; master switch (also toggled from the overlay)
//   separation=0.7   ; flat-window eye distance in world units (VR uses
//                    ; the headset IPD x world_scale instead)
//   cross_eye=0      ; 1 = left-eye image on the right half (cross-eye
//                    ;     viewing); 0 = left on left (parallel viewing)

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <windows.h>
#include <d3d9.h>

#include "psobbvr_probe.hpp"
#include "psobbvr_d3d9ex.hpp"
#include "psobbvr_resolution.hpp"
#include "psobbvr_namelabel.hpp"
#include "psobbvr_vr.hpp"
#include "psobbvr_gamecam.hpp"
#include "psobbvr_cullfov.hpp"
#include "psobbvr_log.hpp"
#include "psobbvr_textsnap.hpp"

namespace stereo {

inline bool enabled = true;
inline float separation = 0.7f;
inline bool cross_eye = false;

// True when the backbuffer was created double wide (set by
// OverridePresentParameters on CreateDevice and Reset).
inline bool double_wide = false;

// Set when render-target creation failed; stereo stays off until Reset.
inline bool create_failed = false;

// Per-frame state, reset after Present.
inline bool composited = false;  // this frame's side-by-side composite is done
inline bool paused = false;      // the game bound its own render target

// The D3D9 device the eye targets belong to (a different device means the
// game re-created it).
inline IDirect3DDevice9* device = nullptr;
// Both eyes render into ONE double-wide target, left eye in the left half,
// selected per draw by viewport only: separate per-eye targets cost
// thousands of SetRenderTarget calls per frame in busy scenes.
inline IDirect3DSurface9* wide_rt = nullptr;  // 2*eye_width x eye_height
inline IDirect3DSurface9* wide_ds = nullptr;
inline IDirect3DSurface9* game_ds = nullptr;  // the game's own depth-stencil
inline UINT eye_width = 0, eye_height = 0;    // ONE eye's size in pixels

// True while wide_rt/wide_ds are bound, so BeginEye can skip the re-bind.
// Cleared wherever anything else gets bound: the game's own
// SetRenderTarget, BindHudLayer, the FinishFrame composite, target release.
inline bool wide_bound = false;

// Per-frame cache of both eyes' view/projection (BeginEye runs per draw per
// eye). Invalidated when an input changes: game_view/game_proj capture,
// each gamecam::Apply, and frame end (fresh WaitPoses pose).
inline D3DMATRIX cached_eye_view[2];
inline D3DMATRIX cached_eye_proj[2];
inline uint32_t eye_cache_stamp = 1;   // bump = invalidate
inline uint32_t eye_cache_seen = 0;    // stamp the cache was built at
inline void InvalidateEyeCache() { eye_cache_stamp++; }

// On a D3D9Ex device the wide target is created shareable; this is its
// shared handle, which the VR backend opens on its D3D11 device.
inline HANDLE wide_shared_handle = nullptr;
// Event query used to flush the eye targets' GPU work before VR submit.
inline IDirect3DQuery9* flush_query = nullptr;

// The HUD texture: screen-space UI and the boxed 3D passes (radar) render
// once into this mono 4:3 texture, which the VR backend shows as a quad
// layer the compositor repositions every display refresh (a HUD baked into
// the eye images swims under reprojection). Shareable like the eye target.
inline IDirect3DTexture9* hud_tex = nullptr;
inline IDirect3DSurface9* hud_rt = nullptr;  // hud_tex level 0
inline IDirect3DSurface9* hud_ds = nullptr;
inline HANDLE hud_shared_handle = nullptr;
inline UINT hud_width = 0, hud_height = 0;
// Per frame (reset in OnFrameEnd): cleared to transparent at the first HUD
// draw; hud_drawn tells the backend the quad has fresh content.
inline bool hud_cleared = false;
inline bool hud_drawn = false;

// Menu world-lock: the below-quad texture for non-gameplay frames. 2D
// drawn before any 3D goes here in draw order over an opaque black clear;
// 2D drawn after 3D uses hud_tex (idle on those frames) as the transparent
// above quad. Same size as hud_tex; shares hud_ds (the 2D z-tests but
// never writes z).
inline IDirect3DTexture9* menu_tex = nullptr;
inline IDirect3DSurface9* menu_rt = nullptr;  // menu_tex level 0
inline HANDLE menu_shared_handle = nullptr;
// Per frame (reset in OnFrameEnd; hud_cleared/hud_drawn serve the above
// quad on menu frames).
inline bool menu_below_cleared = false;
inline bool menu_below_drawn = false;
// Screen rects (640x480 UI space) of the 2D draws routed to the above
// texture in the current BeginScene pass (only content drawn earlier in
// the same pass lies beneath a later draw). A draw sitting on them stays
// above too, or it would be buried behind that content (e.g. a popup's
// selection bar).
struct MenuRect { float x0, y0, x1, y1; };
inline std::vector<MenuRect> menu_above_rects;
// Float region: the rects of every draw that floated at the above quad
// during the previous frame. A draw touching it floats too, so a
// multi-piece panel (the char-select info card) is absorbed piece by
// piece and then stays put, since a floated piece always touches its own
// previous rect.
inline std::vector<MenuRect> menu_float_prev;
inline std::vector<MenuRect> menu_float_accum;
// Projected screen bounds (640x480 UI space) of the menu 3D content (the
// char-select character). Post-3D 2D mostly over it floats on the above
// quad; everything else lies flat on the backdrop. The current frame
// accumulates into _accum; routing tests against _ref, built from earlier
// frames and slightly inflated, so every draw of a frame decides the same
// way (the game draws the screen twice per tick with differing bounds).
// _full = 3D happened with unknown extent (vertex-buffer draw, or a corner
// behind the near plane): treat as the whole screen.
inline MenuRect menu_3d_accum = {};
inline bool menu_3d_accum_valid = false;
inline bool menu_3d_accum_full = false;
// _ref is the union of the last two 30-frame buckets: the idle animation
// swings the character's bounds enough that a single frame's bounds make
// the info card flip between planes.
inline MenuRect menu_3d_hold[2] = {};
inline bool menu_3d_hold_valid[2] = {};
inline bool menu_3d_hold_full[2] = {};
inline int menu_3d_hold_frames = 0;
inline MenuRect menu_3d_ref = {};
inline bool menu_3d_ref_valid = false;
inline bool menu_3d_ref_full = false;
// Set once a real 3D draw (non-RHW, perspective) ran in the current
// BeginScene pass: 2D routes below before it and above after. Reset per
// pass, since each of the game's two passes per tick redraws its backdrop
// first.
inline bool menu_seen_3d = false;
// True while the below target is bound: ApplyHudLayerAlpha leaves the
// draw alone there (the below quad is opaque).
inline bool menu_below_bound = false;

// The game's view matrix (as last set through SetTransform) and the last
// viewport the game applied (after resolution scaling). Width 0 = none
// seen yet.
inline D3DMATRIX game_view = { 1.0f, 0.0f, 0.0f, 0.0f,
                               0.0f, 1.0f, 0.0f, 0.0f,
                               0.0f, 0.0f, 1.0f, 0.0f,
                               0.0f, 0.0f, 0.0f, 1.0f };
inline D3DVIEWPORT9 current_viewport = {};

// The game's projection matrix, tracked the same way. VR replaces it only
// for perspective passes; ortho and RHW UI passes render identically in
// both eyes.
inline D3DMATRIX game_proj = { 1.0f, 0.0f, 0.0f, 0.0f,
                               0.0f, 1.0f, 0.0f, 0.0f,
                               0.0f, 0.0f, 1.0f, 0.0f,
                               0.0f, 0.0f, 0.0f, 1.0f };

// The view matrix the game's own CPU code projects sprites with (before
// the head view is substituted on the device). Captured only from the
// game's scene-view slot (0x00ACBF80, matched by pointer in SetTransform):
// UI passes set other views, and unprojecting through those doubles head
// motion. Valid per frame. Menu screens write their own matrices into the
// same slots, so EffectReprojActive gates menus out separately.
// Also the last perspective projection the game set: world sprites are
// CPU-projected with it even if another projection is bound at the draw.
inline D3DMATRIX cpu_view = game_view;
inline D3DMATRIX last_persp_proj = game_proj;
inline bool cpu_view_valid = false;
inline bool last_persp_proj_valid = false;

inline bool IsPerspective(const D3DMATRIX& m) {
    // w = +/-z: perspective divide. The scale checks exclude the degenerate
    // perspective matrix (_11=_22=0) of the title/menu background pass,
    // which must keep the game's own projection.
    return (m._34 > 0.5f || m._34 < -0.5f) &&
           (m._11 > 1e-6f || m._11 < -1e-6f) &&
           (m._22 > 1e-6f || m._22 < -1e-6f);
}

// True while per-eye view/projection should come from the headset.
inline bool VrDrives() {
    return vrmod::Get()->Ready();
}

// True when the game's viewport is under 25% of its 640x480 screen: a
// 3D-rendered UI element. The minimap radar draws world-space map polygons
// through a private top-down camera (view _43=-2000, _11=_22=2.735) into a
// 128x128 box at 480,64, and its matrices arrive through the same scene
// slots (0xACBF80/0xACBF40) as the real camera, so only the viewport tells
// them apart. The scene is full screen, or 560x365 (65%) with the menu
// open.
inline bool GameViewportIsBoxed() {
    const D3DVIEWPORT9& gvp = resolution::last_game_viewport;
    return (float)gvp.Width * (float)gvp.Height <
           0.25f * resolution::kGameWidth * resolution::kGameHeight;
}

// Field-menu detection for stick navigation: with the menu open the game
// renders the scene into a shrunken viewport (80,0 560x365, ~66% of the
// screen); sub-25% boxes are 3D UI passes (radar). Latched at SetViewport
// and rolled at frame end, because controller::OnFrame runs at BeginScene
// before the game sets that frame's viewport. NPC dialogs do not shrink
// the viewport and are not detected here.
inline bool menu_viewport_seen = false;    // shrunken-scene viewport this frame
inline bool menu_viewport_active = false;  // last presented frame had one
inline void NoteGameViewport(const D3DVIEWPORT9& gvp) {
    const float area = (float)gvp.Width * (float)gvp.Height;
    const float full =
        (float)resolution::kGameWidth * (float)resolution::kGameHeight;
    if (area >= 0.25f * full && area < 0.99f * full)
        menu_viewport_seen = true;
}

// ---- Burst-transition 3D ---------------------------------------------------
//
// The burst tunnel (char select -> lobby, lobby -> game) is a non-gameplay
// screen of CPU-projected additive sprites, projected through an identity
// scene view and the standard scene projection. Menu sparkles share the
// same render states and also sit at fractional coordinates, so burst
// frames are recognized per FRAME, not per draw:
//   - the scene projection is the standard one (p22 1.7641; the main
//     menu/login screens use 1.9201, the loading grid 1.8231), and
//   - the frame has many (~300) additive, z-tested, z-write-off, deep RHW
//     draws at fractional screen coordinates (CPU projection output;
//     authored menu art sits on the half-pixel grid).
// The latch flips only between presented frames, with hysteresis. While
// it holds, the menu world-lock stands down and the sprites are
// reprojected per eye through the identity camera, so the wearer stands
// inside the tunnel; text and the black backdrop stay on the HUD path.
// [vr] burst_3d.
inline int burst_candidates = 0;   // qualifying draws this presented frame
inline bool burst_active = false;  // the frame-level latch

// In-game teleport warp: the same tunnel on gameplay frames. The game
// projects it through a private warp camera, not the scene view, so
// unprojecting through cpu_view puts every sprite behind the eye. While
// the warp latch holds, sprites unproject through a snapshot of the
// head view taken at latch-on, forming a world-locked shell around the
// wearer. A warp frame draws ~360 such sprites and no perspective scene
// polygons, while real gameplay draws hundreds, so combat particles alone
// cannot latch it.
inline int warp_candidates = 0;    // gameplay-frame tunnel candidates
inline int scene_persp_draws = 0;  // non-RHW perspective draws this present
inline bool warp_active = false;
inline D3DMATRIX warp_view = game_view;  // takeover view at latch-on

inline bool BurstActive() {
    return burst_active && vrmod::config.burst_3d;
}

// Called for every non-RHW draw while duplicating - the warp gate's
// scene-geometry half.
inline void NoteSceneDraw() {
    if (IsPerspective(game_proj) && !GameViewportIsBoxed())
        scene_persp_draws++;
}

inline void NoteBurstCandidate(IDirect3DDevice9* dev, const void* data,
                               UINT stride) {
    if (!vrmod::config.burst_3d || !VrDrives())
        return;
    if (data == nullptr || stride < 16 || !last_persp_proj_valid)
        return;
    const bool driving = gamecam::DrivesView();
    // Menu frames need the standard scene projection; gameplay frames
    // (the warp) use the cullfov-patched one, so no test there.
    if (!driving && fabsf(last_persp_proj._22 - 1.7641f) > 0.02f)
        return;
    // Authored UI sits on the half-pixel grid; CPU projection lands
    // anywhere. Vertex 0 is enough.
    const float* v = reinterpret_cast<const float*>(data);
    const float x2 = v[0] * 2.0f, y2 = v[1] * 2.0f;
    if (x2 == floorf(x2) && y2 == floorf(y2))
        return;
    DWORD dest_blend = 0, alpha_blend = 0, z_enable = 0, z_write = 0;
    dev->GetRenderState(D3DRS_DESTBLEND, &dest_blend);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &alpha_blend);
    dev->GetRenderState(D3DRS_ZENABLE, &z_enable);
    dev->GetRenderState(D3DRS_ZWRITEENABLE, &z_write);
    if (dest_blend != D3DBLEND_ONE || alpha_blend == 0 ||
        z_enable == D3DZB_FALSE || z_write != 0)
        return;
    // Fixed 10-unit view-depth floor, not the configurable IsWorldRhw one:
    // the latch thresholds are calibrated against this population.
    const float denom = last_persp_proj._33 + v[2];
    if (denom > -1e-6f)
        return;
    if (-last_persp_proj._43 / denom >= -10.0f)
        return;
    if (driving)
        warp_candidates++;
    else
        burst_candidates++;
}

// ---- Menu world-lock -------------------------------------------------------
//
// Non-gameplay frames (gamecam::DrivesView() false) put their 2D on a
// world-locked screen at an anchor the backend captures from the wearer's
// pose; head-locked content judders under reprojection at 30 FPS.
// 2D before the pass's first 3D draw -> the opaque below quad (menu_tex);
// 3D (char-select character, ship-select scene) -> the eye images; 2D after
// 3D -> hud_tex as the transparent above quad.

inline bool MenuLockActive() {
    // Burst frames skip the menu quads: their sprites are reprojected into
    // the eye images, and the quads would cover them.
    return VrDrives() && vrmod::config.menu_lock && !gamecam::DrivesView() &&
           !BurstActive() &&
           hud_rt != nullptr && menu_rt != nullptr &&
           vrmod::Get()->SupportsMenuLock();
}

// SteamVR composites quad layers over the eye images whatever the order,
// so the char-select character would hide behind the opaque backdrop
// quad. When the pass has backdrop content beneath the 3D, the 3D renders
// into the below texture through the game's own camera (like the radar)
// and the screen becomes the flat game's picture. Ship select draws no 2D
// before its dome, so the dome keeps per-eye rendering.
inline bool MenuFlatten3dNow() {
    return MenuLockActive() && menu_below_drawn && IsPerspective(game_proj);
}

// Per-draw bookkeeping for non-RHW draws on menu frames. A perspective draw
// marks the pass "3D seen" (later 2D goes above). Returns true when the
// draw must be skipped: the flat window's letterbox pass (the only one
// with a viewport wider than 640) adds nothing in VR and would spoil the
// eye images' alpha.
inline bool MenuNoteNonRhwDraw() {
    if (!MenuLockActive())
        return false;
    if (IsPerspective(game_proj)) {
        // Flattened 3D is below content: the pass stays "not seen", so all
        // 2D keeps routing below.
        if (!MenuFlatten3dNow())
            menu_seen_3d = true;
        return false;
    }
    return resolution::last_game_viewport.Width > (DWORD)resolution::kGameWidth;
}

// Called at BeginScene: resets the per-pass routing state (the game
// renders two passes per tick, each redrawing its backdrop first).
inline void OnBeginScenePass() {
    menu_seen_3d = false;
    menu_above_rects.clear();
}

inline void LoadConfig() {
    char path[MAX_PATH];
    if (!GetCurrentDirectoryA(MAX_PATH, path))
        return;
    strcat_s(path, "\\psobbvr.ini");
    enabled = GetPrivateProfileIntA("stereo", "enabled", 1, path) != 0;
    cross_eye = GetPrivateProfileIntA("stereo", "cross_eye", 0, path) != 0;
    char value[32];
    if (GetPrivateProfileStringA("stereo", "separation", "", value, sizeof(value), path)) {
        const float parsed = (float)atof(value);
        if (parsed > 0.0f && parsed < 100.0f)
            separation = parsed;
    }
}

// CreateDevice and Reset, after the resolution override: with stereo on,
// double the backbuffer width so each eye gets a full-resolution half.
// (Width 0 is never seen here; the resolution override sets it.)
inline void OverridePresentParameters(D3DPRESENT_PARAMETERS& params) {
    double_wide = false;
    if (!enabled || !params.Windowed || params.BackBufferWidth == 0)
        return;
    params.BackBufferWidth *= 2;
    double_wide = true;
}

inline void ReleaseTargets() {
    if (wide_rt) { wide_rt->Release(); wide_rt = nullptr; }
    if (wide_ds) { wide_ds->Release(); wide_ds = nullptr; }
    wide_shared_handle = nullptr;  // owned by the surface, not closed here
    wide_bound = false;
    InvalidateEyeCache();
    if (hud_rt) { hud_rt->Release(); hud_rt = nullptr; }
    if (hud_tex) { hud_tex->Release(); hud_tex = nullptr; }
    if (hud_ds) { hud_ds->Release(); hud_ds = nullptr; }
    hud_shared_handle = nullptr;
    hud_width = hud_height = 0;
    hud_cleared = hud_drawn = false;
    if (menu_rt) { menu_rt->Release(); menu_rt = nullptr; }
    if (menu_tex) { menu_tex->Release(); menu_tex = nullptr; }
    menu_shared_handle = nullptr;
    menu_below_cleared = menu_below_drawn = false;
    menu_seen_3d = menu_below_bound = false;
    menu_above_rects.clear();
    menu_float_prev.clear();
    menu_float_accum.clear();
    if (game_ds) { game_ds->Release(); game_ds = nullptr; }
    if (flush_query) { flush_query->Release(); flush_query = nullptr; }
    eye_width = eye_height = 0;
    device = nullptr;
    create_failed = false;
    composited = false;
    paused = false;
    current_viewport.Width = 0;
}

// Hand the current shared handle to the VR backend (no-op until both the
// target and the VR runtime exist). Called after target creation and after
// a live VR start from the panel.
inline void AttachToVr() {
    if (!vrmod::Get()->Ready() || wide_shared_handle == nullptr || wide_rt == nullptr)
        return;
    // One double-wide texture plus the per-eye size; the backend slices it.
    vrmod::Get()->AttachEyeTextures(wide_shared_handle, eye_width, eye_height);
    // The HUD texture must come after the eyes (the backend reuses their
    // swapchain format). A backend without quad layers declines.
    if (hud_shared_handle != nullptr && hud_rt != nullptr)
        vrmod::Get()->AttachHudTexture(hud_shared_handle, hud_width, hud_height);
    // The menu below-quad texture, likewise.
    if (menu_shared_handle != nullptr && menu_rt != nullptr)
        vrmod::Get()->AttachMenuTexture(menu_shared_handle, hud_width, hud_height);
}

// Create the double-wide render target + depth surface to match the current
// backbuffer. Returns true when the target is ready to use.
inline bool EnsureTargets(IDirect3DDevice9* dev) {
    if (dev != device)
        ReleaseTargets();
    if (create_failed)
        return false;
    if (wide_rt != nullptr)
        return true;

    IDirect3DSurface9* backbuffer = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer))) {
        create_failed = true;
        return false;
    }
    D3DSURFACE_DESC bb_desc;
    backbuffer->GetDesc(&bb_desc);
    backbuffer->Release();

    D3DFORMAT ds_format = D3DFMT_D24S8;
    if (SUCCEEDED(dev->GetDepthStencilSurface(&game_ds)) && game_ds != nullptr) {
        D3DSURFACE_DESC ds_desc;
        game_ds->GetDesc(&ds_desc);
        ds_format = ds_desc.Format;
    }

    // One eye = half a double-wide backbuffer, else the whole backbuffer;
    // the target is twice that wide. On a D3D9Ex device it is created
    // shareable in A8R8G8B8 (opened by D3D11 as BGRA) for the VR backend.
    eye_width = double_wide ? bb_desc.Width / 2 : bb_desc.Width;
    eye_height = bb_desc.Height;
    const bool shareable = d3d9ex::active;
    const D3DFORMAT rt_format = shareable ? D3DFMT_A8R8G8B8 : bb_desc.Format;
    {
        HRESULT hr_rt = dev->CreateRenderTarget(eye_width * 2, bb_desc.Height, rt_format,
                                                D3DMULTISAMPLE_NONE, 0, FALSE, &wide_rt,
                                                shareable ? &wide_shared_handle : nullptr);
        HRESULT hr_ds = SUCCEEDED(hr_rt)
            ? dev->CreateDepthStencilSurface(eye_width * 2, bb_desc.Height, ds_format,
                                             D3DMULTISAMPLE_NONE, 0, TRUE, &wide_ds, nullptr)
            : D3D_OK;
        if (FAILED(hr_rt) || FAILED(hr_ds)) {
            diag::Log("stereo: wide target creation FAILED rt=0x%08X ds=0x%08X (%ux%u fmt=%d dsfmt=%d shareable=%d)",
                      hr_rt, hr_ds, eye_width * 2, bb_desc.Height, rt_format, ds_format, shareable ? 1 : 0);
            ReleaseTargets();
            create_failed = true;
            return false;
        }
    }
    probe::Log("stereo: created double-wide target %ux%u (eye %ux%u, backbuffer %ux%u, double_wide=%d, shared handle %p)",
               eye_width * 2, bb_desc.Height, eye_width, eye_height,
               bb_desc.Width, bb_desc.Height, double_wide ? 1 : 0, wide_shared_handle);
    // The HUD texture. Failure is non-fatal: the HUD stays in the eye
    // images (HudLayerActive() checks hud_rt).
    if (shareable) {
        hud_width = (UINT)vrmod::config.hud_tex_width;
        hud_height = hud_width * 3 / 4;  // the UI's 4:3 shape
        const HRESULT hr_tex = dev->CreateTexture(
            hud_width, hud_height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT, &hud_tex, &hud_shared_handle);
        const HRESULT hr_surf = SUCCEEDED(hr_tex)
            ? hud_tex->GetSurfaceLevel(0, &hud_rt) : D3D_OK;
        const HRESULT hr_hds = SUCCEEDED(hr_tex) && SUCCEEDED(hr_surf)
            ? dev->CreateDepthStencilSurface(hud_width, hud_height, ds_format,
                                             D3DMULTISAMPLE_NONE, 0, TRUE, &hud_ds, nullptr)
            : D3D_OK;
        if (FAILED(hr_tex) || FAILED(hr_surf) || FAILED(hr_hds)) {
            diag::Log("stereo: hud texture creation FAILED tex=0x%08X surf=0x%08X ds=0x%08X (%ux%u)",
                      hr_tex, hr_surf, hr_hds, hud_width, hud_height);
            if (hud_rt) { hud_rt->Release(); hud_rt = nullptr; }
            if (hud_tex) { hud_tex->Release(); hud_tex = nullptr; }
            if (hud_ds) { hud_ds->Release(); hud_ds = nullptr; }
            hud_shared_handle = nullptr;
            hud_width = hud_height = 0;
        } else {
            probe::Log("stereo: created hud texture %ux%u (shared handle %p)",
                       hud_width, hud_height, hud_shared_handle);
            // The menu below-quad texture, same size and format, sharing
            // hud_ds. Non-fatal (MenuLockActive checks menu_rt).
            const HRESULT hr_mtex = dev->CreateTexture(
                hud_width, hud_height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                D3DPOOL_DEFAULT, &menu_tex, &menu_shared_handle);
            const HRESULT hr_msurf = SUCCEEDED(hr_mtex)
                ? menu_tex->GetSurfaceLevel(0, &menu_rt) : D3D_OK;
            if (FAILED(hr_mtex) || FAILED(hr_msurf)) {
                diag::Log("stereo: menu texture creation FAILED tex=0x%08X surf=0x%08X",
                          hr_mtex, hr_msurf);
                if (menu_rt) { menu_rt->Release(); menu_rt = nullptr; }
                if (menu_tex) { menu_tex->Release(); menu_tex = nullptr; }
                menu_shared_handle = nullptr;
            } else {
                probe::Log("stereo: created menu texture %ux%u (shared handle %p)",
                           hud_width, hud_height, menu_shared_handle);
            }
        }
    }
    device = dev;
    AttachToVr();
    return true;
}

// True while game draw/clear calls should be mirrored into the eye targets.
// A double-wide backbuffer always needs it (a mono frame would cover half
// the window). The call sites also check resolution::passthrough, so the
// overlay's own draws are never duplicated.
inline bool WantsDuplication() {
    return (enabled || double_wide) && !composited && !paused && !create_failed;
}

// One eye's VR view matrix. With the camera takeover driving, the game's
// view already holds the head pose (minus roll), so the eye view is built
// straight from the tracked pose (inverse of eye-to-head x world-from-head),
// which also restores roll. Otherwise the head/eye correction is appended
// to the game's view.
inline void ComputeVrEyeView(int eye, D3DMATRIX& eye_view) {
    if (gamecam::DrivesView()) {
        D3DMATRIX eye_to_head, head_world;
        vrmod::Get()->GetEyeToHead(eye, eye_to_head);
        gamecam::GetWorldFromHead(head_world);
        eye_view = vrmod::RigidInverse(vrmod::Multiply(eye_to_head, head_world));
    } else {
        D3DMATRIX append;
        vrmod::Get()->GetEyeViewAppend(eye, append);
        // Menu and burst frames are placed relative to the menu anchor, not
        // the raw tracking origin, so menu 3D stands in a fixed relation to
        // the world-locked menu screen (a wearer at the anchor sees the flat
        // game's framing). append is head_inv * eye_to_head_inv in game
        // units; pre-multiplying the anchor makes the head relative to it.
        // Without an anchor the raw append is the fallback.
        D3DMATRIX anchor;
        if ((MenuLockActive() || BurstActive()) &&
            vrmod::Get()->GetMenuAnchor(anchor)) {
            append = vrmod::Multiply(anchor, append);
            // Scaling the translation (wearer offset + eye baseline) makes
            // content read menu_depth_scale times nearer and smaller while
            // the framing at the anchor stays: it pulls the char-select
            // character in between the two menu quads.
            const float s = vrmod::config.menu_depth_scale;
            if (s != 1.0f) {
                append._41 *= s;
                append._42 *= s;
                append._43 *= s;
            }
        }
        // Row-vector: p_view = p_world * game_view, then head/eye correction.
        eye_view = vrmod::Multiply(game_view, append);
    }
}

// One eye's VR projection: the headset's frustum (x/y rows) with the
// game's depth mapping (_33/_43 from depth_src), which its fog relies on.
inline void ComputeVrEyeProj(int eye, const D3DMATRIX& depth_src, D3DMATRIX& eye_proj) {
    vrmod::Get()->GetEyeProjection(eye, eye_proj);
    eye_proj._33 = depth_src._33;
    eye_proj._43 = depth_src._43;
}

// Bind the wide target unless it is already bound (see wide_bound).
inline void BindWideTarget(IDirect3DDevice9* dev) {
    if (wide_bound)
        return;
    dev->SetRenderTarget(0, wide_rt);
    dev->SetDepthStencilSurface(wide_ds);
    wide_bound = true;
}

// Place a game-space viewport into one eye's half, clamped inside it (the
// title screen can size one from the double-wide window, and an
// out-of-bounds SetViewport fails).
inline void SetEyeViewport(IDirect3DDevice9* dev, D3DVIEWPORT9 vp, int eye) {
    if (vp.X > eye_width) vp.X = eye_width;
    if (vp.X + vp.Width > eye_width) vp.Width = eye_width - vp.X;
    if (vp.Y > eye_height) vp.Y = eye_height;
    if (vp.Y + vp.Height > eye_height) vp.Height = eye_height - vp.Y;
    if (vp.Width == 0 || vp.Height == 0)
        return;
    vp.X += (DWORD)(eye * eye_width);
    dev->SetViewport(&vp);
}

// Select one eye for the next draw: its half of the wide target by
// viewport, plus its matrices. With VR, perspective passes get the cached
// per-eye view and headset projection; other passes (ortho, RHW UI) keep
// the game's matrices and render identically in both eyes.
inline void BeginEye(IDirect3DDevice9* dev, int eye) {
    BindWideTarget(dev);
    const LONG ex = (LONG)(eye * eye_width);  // this eye's half origin
    const bool vr = VrDrives();
    if (vr && IsPerspective(game_proj)) {
        if (GameViewportIsBoxed()) {
            // 3D-rendered UI (the radar): keep the game's own camera and
            // projection (its polygons are in world coordinates), and place
            // its box per eye with the same mapping the RHW UI gets.
            dev->SetTransform(D3DTS_VIEW, &game_view);
            dev->SetTransform(D3DTS_PROJECTION, &game_proj);
            const D3DVIEWPORT9& gvp = resolution::last_game_viewport;
            float sx, ox, sy, oy;
            vrmod::Get()->GetHudRemap(eye, eye_width, eye_height, sx, ox, sy, oy);
            LONG x = (LONG)(gvp.X * sx + ox + 0.5f);
            LONG y = (LONG)(gvp.Y * sy + oy + 0.5f);
            LONG w = (LONG)(gvp.Width * sx + 0.5f);
            LONG h = (LONG)(gvp.Height * sy + 0.5f);
            if (x < 0) { w += x; x = 0; }
            if (y < 0) { h += y; y = 0; }
            if (x + w > (LONG)eye_width) w = (LONG)eye_width - x;
            if (y + h > (LONG)eye_height) h = (LONG)eye_height - y;
            if (w > 0 && h > 0) {
                D3DVIEWPORT9 box = { (DWORD)(x + ex), (DWORD)y, (DWORD)w, (DWORD)h,
                                     gvp.MinZ, gvp.MaxZ };
                dev->SetViewport(&box);
            }
            return;
        }
        // The headset projection needs the full eye half as viewport, even
        // when the game shrinks its own (80,0 560x365 with the menu open).
        // Only the game's depth range is kept.
        D3DVIEWPORT9 full = { (DWORD)ex, 0, eye_width, eye_height, 0.0f, 1.0f };
        if (current_viewport.Width != 0) {
            full.MinZ = current_viewport.MinZ;
            full.MaxZ = current_viewport.MaxZ;
        }
        dev->SetViewport(&full);
        if (eye_cache_seen != eye_cache_stamp) {
            for (int e = 0; e < 2; e++) {
                ComputeVrEyeView(e, cached_eye_view[e]);
                ComputeVrEyeProj(e, game_proj, cached_eye_proj[e]);
            }
            eye_cache_seen = eye_cache_stamp;
        }
        dev->SetTransform(D3DTS_VIEW, &cached_eye_view[eye]);
        dev->SetTransform(D3DTS_PROJECTION, &cached_eye_proj[eye]);
        return;
    }
    if (current_viewport.Width != 0) {
        SetEyeViewport(dev, current_viewport, eye);
    } else {
        // No game viewport seen yet: never fall through to the wide
        // target's default viewport (it spans BOTH halves).
        D3DVIEWPORT9 half = { (DWORD)ex, 0, eye_width, eye_height, 0.0f, 1.0f };
        dev->SetViewport(&half);
    }
    if (vr)  // non-perspective pass: undo a possible per-eye projection left
        dev->SetTransform(D3DTS_PROJECTION, &game_proj);
    D3DMATRIX eye_view = game_view;
    if (enabled)  // stereo toggled off on a double-wide backbuffer: no offset
        eye_view._41 += (eye == 0 ? +separation : -separation) * 0.5f;
    dev->SetTransform(D3DTS_VIEW, &eye_view);
}

// True when screen-space UI vertices get the per-eye virtual-screen remap
// (in place of the resolution override's plain scaling).
inline bool HudRemapActive() {
    return VrDrives();
}

inline std::vector<uint8_t> hud_scratch;

// Copy screen-space UI vertices with x/y mapped through this eye's virtual
// screen: game 640x480 in, eye-target pixels out; z and rhw unchanged.
// Valid until the next call (D3D consumes user-pointer data in the draw).
inline const void* RemapHudVertices(int eye, const void* data, UINT vertex_count, UINT stride) {
    float sx, ox, sy, oy;
    vrmod::Get()->GetHudRemap(eye, eye_width, eye_height, sx, ox, sy, oy);
    static bool logged[2] = {};
    if (probe::enabled && !logged[eye & 1]) {
        logged[eye & 1] = true;
        probe::Log("hud-remap eye %d: x' = %.4f*x %+.2f  y' = %.4f*y %+.2f (target %ux%u, dist %.2fm width %.0fdeg)",
                   eye, sx, ox, sy, oy, eye_width, eye_height,
                   vrmod::config.hud_distance_m, vrmod::config.hud_width_deg);
    }
    const auto* src = static_cast<const uint8_t*>(data);
    hud_scratch.assign(src, src + size_t(vertex_count) * stride);
    uint8_t* vertex = hud_scratch.data();
    // Pretransformed vertices are in wide-target pixels (the viewport only
    // clips them), so the right eye needs an explicit x offset.
    const float ex = (float)(eye * eye_width);
    for (UINT i = 0; i < vertex_count; i++, vertex += stride) {
        float* xy = reinterpret_cast<float*>(vertex);
        xy[0] = xy[0] * sx + ox + ex;
        xy[1] = xy[1] * sy + oy;
    }
    return hud_scratch.data();
}

// ---- Dedicated HUD quad layer ----------------------------------------------
//
// When active, RHW UI and the boxed 3D passes (radar) render once into
// hud_tex, which the VR backend shows as a quad layer at
// hud_distance_m / hud_width_deg. The per-eye paths above serve the OpenVR
// backend and hud_layer=0.

inline bool HudLayerActive() {
    // Gameplay frames only (the camera takeover driving); menu frames use
    // the menu world-lock. DrivesView is settled before the frame's first
    // draw, so the gate is stable within a frame.
    return VrDrives() && vrmod::config.hud_layer && hud_rt != nullptr &&
           gamecam::DrivesView() && vrmod::Get()->SupportsHudLayer();
}

inline UINT VertexCountForPrimitives(D3DPRIMITIVETYPE t, UINT prims) {
    switch (t) {
    case D3DPT_POINTLIST:     return prims;
    case D3DPT_LINELIST:      return prims * 2;
    case D3DPT_LINESTRIP:     return prims + 1;
    case D3DPT_TRIANGLELIST:  return prims * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:   return prims + 2;
    default:                  return 0;
    }
}

// 3D menu content with unknown screen extent (vertex-buffer draw) - treat
// as covering the whole screen.
inline void NoteMenu3dRectUnknown() {
    menu_3d_accum_full = true;
}

// Accumulate the projected screen bounds (640x480 UI space) of one menu 3D
// draw: its bounding-box corners through world * game_view * game_proj and
// the game's viewport (still the game's own camera here; Duplicate applies
// the per-eye matrices later).
inline void NoteMenu3dRect(IDirect3DDevice9* dev, const void* data,
                           UINT vertex_count, UINT stride) {
    if (menu_3d_accum_full || data == nullptr || vertex_count == 0 ||
        !IsPerspective(game_proj))
        return;
    float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
    const auto* v = static_cast<const uint8_t*>(data);
    for (UINT i = 0; i < vertex_count; i++, v += stride) {
        const float* p = reinterpret_cast<const float*>(v);
        for (int k = 0; k < 3; k++) {
            if (p[k] < mn[k]) mn[k] = p[k];
            if (p[k] > mx[k]) mx[k] = p[k];
        }
    }
    D3DMATRIX world;
    dev->GetTransform(D3DTS_WORLD, &world);
    const D3DMATRIX wvp =
        vrmod::Multiply(vrmod::Multiply(world, game_view), game_proj);
    const D3DVIEWPORT9& gvp = resolution::last_game_viewport;
    MenuRect r = { 1e9f, 1e9f, -1e9f, -1e9f };
    for (int c = 0; c < 8; c++) {
        const float x = (c & 1) ? mx[0] : mn[0];
        const float y = (c & 2) ? mx[1] : mn[1];
        const float z = (c & 4) ? mx[2] : mn[2];
        const float cw = x * wvp._14 + y * wvp._24 + z * wvp._34 + wvp._44;
        if (cw <= 1e-4f) {
            // A corner behind the near plane - screen bounds undefined.
            menu_3d_accum_full = true;
            return;
        }
        const float cx = (x * wvp._11 + y * wvp._21 + z * wvp._31 + wvp._41) / cw;
        const float cy = (x * wvp._12 + y * wvp._22 + z * wvp._32 + wvp._42) / cw;
        const float sx = (cx * 0.5f + 0.5f) * gvp.Width + gvp.X;
        const float sy = (-cy * 0.5f + 0.5f) * gvp.Height + gvp.Y;
        if (sx < r.x0) r.x0 = sx;
        if (sy < r.y0) r.y0 = sy;
        if (sx > r.x1) r.x1 = sx;
        if (sy > r.y1) r.y1 = sy;
    }
    if (!menu_3d_accum_valid) {
        menu_3d_accum = r;
        menu_3d_accum_valid = true;
        return;
    }
    if (r.x0 < menu_3d_accum.x0) menu_3d_accum.x0 = r.x0;
    if (r.y0 < menu_3d_accum.y0) menu_3d_accum.y0 = r.y0;
    if (r.x1 > menu_3d_accum.x1) menu_3d_accum.x1 = r.x1;
    if (r.y1 > menu_3d_accum.y1) menu_3d_accum.y1 = r.y1;
}

// Fraction of rect r's area lying over the menu 3D bounds reference.
// Routing thresholds it with config.menu_float_overlap: the info card is
// mostly over the character and floats; a thin line crossing it stays flat.
inline float MenuRectOver3dFraction(const MenuRect& r) {
    if (menu_3d_ref_full)
        return 1.0f;
    if (!menu_3d_ref_valid)
        return 0.0f;
    const float area = (r.x1 - r.x0) * (r.y1 - r.y0);
    if (area <= 0.0f)
        return 0.0f;
    const float w = (menu_3d_ref.x1 < r.x1 ? menu_3d_ref.x1 : r.x1) -
                    (menu_3d_ref.x0 > r.x0 ? menu_3d_ref.x0 : r.x0);
    const float h = (menu_3d_ref.y1 < r.y1 ? menu_3d_ref.y1 : r.y1) -
                    (menu_3d_ref.y0 > r.y0 ? menu_3d_ref.y0 : r.y0);
    if (w <= 0.0f || h <= 0.0f)
        return 0.0f;
    return (w * h) / area;
}

inline bool MenuTouchesFloatRegion(const MenuRect& r) {
    const float pad = 4.0f;  // abutting 9-slice pieces share edges, not area
    for (const MenuRect& a : menu_float_prev)
        if (r.x0 - pad < a.x1 && r.x1 + pad > a.x0 &&
            r.y0 - pad < a.y1 && r.y1 + pad > a.y0)
            return true;
    return false;
}

// Fraction of rect r covered by the most-overlapping above rect of this
// pass. An area fraction, not a plain overlap test: a row's fill bar only
// clips its label drawn just before it, while a popup's selection bar sits
// wholly on its popup box.
inline float MenuRectAboveCoverage(const MenuRect& r) {
    const float area = (r.x1 - r.x0) * (r.y1 - r.y0);
    if (area <= 0.0f)
        return 0.0f;
    float best = 0.0f;
    for (const MenuRect& a : menu_above_rects) {
        const float w = (a.x1 < r.x1 ? a.x1 : r.x1) - (a.x0 > r.x0 ? a.x0 : r.x0);
        const float h = (a.y1 < r.y1 ? a.y1 : r.y1) - (a.y0 > r.y0 ? a.y0 : r.y0);
        if (w > 0.0f && h > 0.0f) {
            const float f = (w * h) / area;
            if (f > best)
                best = f;
        }
    }
    return best;
}

// Bounding rectangle of an RHW draw's vertices, in the game's 640x480 UI
// space (compute from the ORIGINAL vertex data, before any scaling).
inline MenuRect ComputeRhwRect(const void* data, UINT vertex_count, UINT stride) {
    MenuRect r = { 1e9f, 1e9f, -1e9f, -1e9f };
    const auto* v = static_cast<const uint8_t*>(data);
    for (UINT i = 0; i < vertex_count; i++, v += stride) {
        const float* xy = reinterpret_cast<const float*>(v);
        if (xy[0] < r.x0) r.x0 = xy[0];
        if (xy[1] < r.y0) r.y0 = xy[1];
        if (xy[0] > r.x1) r.x1 = xy[0];
        if (xy[1] > r.y1) r.y1 = xy[1];
    }
    return r;
}


// Bind the HUD texture (or on menu frames one of the two menu textures) as
// the render target, clearing it at its first use each frame. boxed = a
// 3D-rendered UI pass (radar): its viewport is scaled 640x480 -> texture
// and its own view/projection restored. RHW draws get the full texture as
// viewport (they arrive in full-screen coordinates even under a boxed
// viewport). mark_drawn = false for clears, which do not count as content.
// rect = the draw's UI-space bounds, for menu routing.
inline void BindHudLayer(IDirect3DDevice9* dev, bool boxed, bool mark_drawn,
                         const MenuRect* rect = nullptr) {
    // Menu frames: the opaque below texture until the pass's first 3D draw,
    // the transparent hud texture (the above quad) after it.
    menu_below_bound = MenuLockActive() && !menu_seen_3d;
    // Post-3D 2D also goes below unless it must stay in front of the 3D:
    // it overlaps the 3D content's screen bounds, sits mostly on content
    // already routed above this pass, or touches last frame's float
    // region. The compositor cannot add, so additive art on the above quad
    // would vanish; below, in draw order, it matches the flat game. Only
    // when the below quad already has real content this frame, and never
    // for clears.
    float tr_over = -1.0f, tr_cover = -1.0f;
    int tr_touch = -1, tr_wide = -1;
    if (!menu_below_bound && MenuLockActive() && !boxed && mark_drawn &&
        menu_below_drawn && vrmod::config.glow_below && rect != nullptr) {
        // Screen-spanning pieces (border lines) float only on their own
        // overlap fraction, never by contact or ride-along.
        const bool wide =
            (rect->x1 - rect->x0) >= 0.7f * resolution::kGameWidth ||
            (rect->y1 - rect->y0) >= 0.7f * resolution::kGameHeight;
        tr_over = MenuRectOver3dFraction(*rect);
        bool floats = tr_over >= vrmod::config.menu_float_overlap;
        if (!floats && !wide) {
            tr_cover = MenuRectAboveCoverage(*rect);
            tr_touch = MenuTouchesFloatRegion(*rect) ? 1 : 0;
            floats = tr_cover >= 0.7f || tr_touch == 1;
        }
        tr_wide = wide ? 1 : 0;
        menu_below_bound = !floats;
    }
    // A draw staying above claims its footprint for later draws this pass
    // (menu_above_rects) and for next frame's float region.
    if (!menu_below_bound && MenuLockActive() && !boxed && mark_drawn &&
        rect != nullptr) {
        menu_above_rects.push_back(*rect);
        menu_float_accum.push_back(*rect);
    }
    dev->SetRenderTarget(0, menu_below_bound ? menu_rt : hud_rt);
    dev->SetDepthStencilSurface(hud_ds);
    wide_bound = false;  // next BeginEye re-binds the wide target
    if (menu_below_bound) {
        if (!menu_below_cleared) {
            menu_below_cleared = true;
            D3DVIEWPORT9 full = { 0, 0, hud_width, hud_height, 0.0f, 1.0f };
            dev->SetViewport(&full);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
        }
        if (mark_drawn)
            menu_below_drawn = true;
    } else {
        if (!hud_cleared) {
            hud_cleared = true;
            D3DVIEWPORT9 full = { 0, 0, hud_width, hud_height, 0.0f, 1.0f };
            dev->SetViewport(&full);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x00000000, 1.0f, 0);
        }
        if (mark_drawn)
            hud_drawn = true;
    }
    if (boxed) {
        const D3DVIEWPORT9& gvp = resolution::last_game_viewport;
        const float sx = hud_width / resolution::kGameWidth;
        const float sy = hud_height / resolution::kGameHeight;
        D3DVIEWPORT9 box = { (DWORD)(gvp.X * sx + 0.5f), (DWORD)(gvp.Y * sy + 0.5f),
                             (DWORD)(gvp.Width * sx + 0.5f), (DWORD)(gvp.Height * sy + 0.5f),
                             gvp.MinZ, gvp.MaxZ };
        if (box.Width > 0 && box.Height > 0 &&
            box.X + box.Width <= hud_width && box.Y + box.Height <= hud_height)
            dev->SetViewport(&box);
        // The pass's private camera arrived through the normal SetTransform
        // slots, so game_view/game_proj hold it right now.
        dev->SetTransform(D3DTS_VIEW, &game_view);
        dev->SetTransform(D3DTS_PROJECTION, &game_proj);
    } else {
        D3DVIEWPORT9 full = { 0, 0, hud_width, hud_height, 0.0f, 1.0f };
        if (current_viewport.Width != 0) {
            full.MinZ = current_viewport.MinZ;
            full.MaxZ = current_viewport.MaxZ;
        }
        dev->SetViewport(&full);
    }
}

inline std::vector<uint8_t> hud_layer_scratch;

// Plain 640x480 -> HUD-texture scaling for screen-space vertices. Valid
// until the next call.
inline const void* ScaleHudLayerVertices(const void* data, UINT vertex_count, UINT stride) {
    const float sx = hud_width / resolution::kGameWidth;
    const float sy = hud_height / resolution::kGameHeight;
    const auto* src = static_cast<const uint8_t*>(data);
    hud_layer_scratch.assign(src, src + size_t(vertex_count) * stride);
    uint8_t* vertex = hud_layer_scratch.data();
    for (UINT i = 0; i < vertex_count; i++, vertex += stride) {
        float* xy = reinterpret_cast<float*>(vertex);
        xy[0] *= sx;
        xy[1] *= sy;
    }
    return hud_layer_scratch.data();
}

// Coverage alpha for draws into the quad textures. The quads composite by
// alpha, which the game's UI never writes meaningfully, but its blending
// over a transparent-black clear leaves the color premultiplied, so only
// alpha needs fixing:
//   standard draws: a_out = a_src + a_dst*(1-a_src)  (ONE/INVSRCALPHA)
//   additive draws: a_out = a_dst (ZERO/ONE) - additive light occludes
//     nothing. SteamVR composites by rgb*alpha, so additive glow over empty
//     quad regions stays invisible; claiming coverage instead shows
//     additive sprites on the gameplay HUD as black boxes. Additive menu
//     art is routed into the opaque below texture instead (BindHudLayer).
// Other dest blends take the standard branch (approximate).
inline DWORD hud_saved_sep, hud_saved_srcba, hud_saved_dstba;
inline bool hud_alpha_forced = false;
// Draws with blending AND alpha test off write raw alpha, and the pre-game
// screens' textures carry alpha 0 in visible regions, so those draws get
// alpha forced to 1 through texture stage 0 (alpha ops only).
// Alpha-tested draws keep their alpha (cutout sprites).
inline DWORD hud_saved_alphaop, hud_saved_alphaarg1, hud_saved_tfactor;
inline bool hud_alpha_tss_forced = false;

// Force meaningful coverage around one draw into an alpha-composited
// target: the HUD/above quad texture, or the eye images on a menu frame.
inline void ApplyCoverageAlpha(IDirect3DDevice9* dev) {
    DWORD alpha_blend = 0;
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &alpha_blend);
    hud_alpha_forced = alpha_blend != 0;
    if (!hud_alpha_forced) {
        DWORD alpha_test = 0;
        dev->GetRenderState(D3DRS_ALPHATESTENABLE, &alpha_test);
        if (alpha_test == 0) {
            hud_alpha_tss_forced = true;
            dev->GetTextureStageState(0, D3DTSS_ALPHAOP, &hud_saved_alphaop);
            dev->GetTextureStageState(0, D3DTSS_ALPHAARG1, &hud_saved_alphaarg1);
            dev->GetRenderState(D3DRS_TEXTUREFACTOR, &hud_saved_tfactor);
            dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
            dev->SetRenderState(D3DRS_TEXTUREFACTOR, 0xFFFFFFFF);
        }
        return;
    }
    dev->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &hud_saved_sep);
    dev->GetRenderState(D3DRS_SRCBLENDALPHA, &hud_saved_srcba);
    dev->GetRenderState(D3DRS_DESTBLENDALPHA, &hud_saved_dstba);
    DWORD dest_blend = 0;
    dev->GetRenderState(D3DRS_DESTBLEND, &dest_blend);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    if (dest_blend == D3DBLEND_ONE) {
        dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ZERO);
        dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_ONE);
    } else {
        dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
        dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    }
}

inline void ApplyHudLayerAlpha(IDirect3DDevice9* dev) {
    // The below quad is opaque: its alpha is never read, so leave the
    // draw's states alone.
    if (menu_below_bound) {
        hud_alpha_forced = false;
        hud_alpha_tss_forced = false;
        return;
    }
    ApplyCoverageAlpha(dev);
}

inline void RestoreHudLayerAlpha(IDirect3DDevice9* dev) {
    if (hud_alpha_tss_forced) {
        hud_alpha_tss_forced = false;
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP, hud_saved_alphaop);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, hud_saved_alphaarg1);
        dev->SetRenderState(D3DRS_TEXTUREFACTOR, hud_saved_tfactor);
    }
    if (!hud_alpha_forced)
        return;
    hud_alpha_forced = false;
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, hud_saved_sep);
    dev->SetRenderState(D3DRS_SRCBLENDALPHA, hud_saved_srcba);
    dev->SetRenderState(D3DRS_DESTBLENDALPHA, hud_saved_dstba);
}

// ---- CPU-projected world sprites -------------------------------------------
//
// The game draws glows, flares and particles by projecting them on the CPU
// and issuing RHW quads, the same vertex format as the 2D UI. In VR they
// must be reprojected into the world instead of going to the HUD.
//
// Every RHW draw carries rhw = 1.0, so rhw says nothing; the view depth is
// in z (invertible through the game's projection; world glows sit at
// z 0.93..0.995 = 15..150 units, one depth per quad). World sprites are
// additive (dest blend ONE) with the depth test on; UI uses standard alpha
// blending (alpha-blended world sprites have their own rule). Combat text
// is intercepted before this classifier (IsCombatText).

// ---- Combat-text placement -------------------------------------------------
//
// The damage/MISS/XP popups are floating-text objects that keep their
// world position (psobbvr_textsnap.hpp). Their draw method re-projects it
// each frame through the game's internal camera copy, which lags the head
// pose, so the screen output cannot be used directly in VR.
//
// The textsnap hook brackets that draw method, which issues each glyph
// inside it as its own 4-vertex DrawPrimitiveUP, so a draw inside the
// bracket belongs to that popup and nothing else is text. Each vertex is
// unprojected through the same text camera at the popup's true depth and
// re-projected per eye: the camera lag cancels, and the game's layout,
// jitter and float-up survive (scaled to stock angular size, see
// ProjectCombatTextVertices). Without the hook, text stays on the HUD.
// Combat-text draws go out with the z test off.

inline float current_text_depth = -1.0f;  // set per bracketed draw
inline float current_text_target_x = 0.0f;  // target's BARE projection through
inline float current_text_target_y = 0.0f;  //   the game's text camera
// Name labels (psobbvr_namelabel.hpp): the label's world anchor. When set,
// ProjectLabelVertices lays the draw out as a billboard around it, using
// no game camera state.
inline bool current_text_anchor_valid = false;
inline float current_text_anchor_world[3] = {};

// General 4x4 matrix inverse (cofactor expansion). False when singular.
inline bool InvertMatrix(const D3DMATRIX& mm, D3DMATRIX& outm) {
    const float* m = &mm._11;
    float inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] +
             m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] -
             m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] +
             m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] -
              m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] -
             m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] +
             m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] -
             m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] +
              m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] +
             m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] -
             m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] +
              m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] -
              m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] -
             m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] +
             m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] -
              m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] +
              m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det > -1e-20f && det < 1e-20f)
        return false;
    det = 1.0f / det;
    float* o = &outm._11;
    for (int i = 0; i < 16; i++)
        o[i] = inv[i] * det;
    return true;
}

// Derived per-frame text-camera data, rebuilt whenever the textsnap hook
// captures a new frame's 2D-context transform.
struct TextCam {
    int cam_frame = -1;
    bool valid = false;
    D3DMATRIX fwd;    // world*view*proj (row-vector convention)
    D3DMATRIX inv;    // its inverse
    float campos[3];  // camera position in world space
};
inline TextCam text_cam;

inline void UpdateTextCam() {
    const textsnap::FrameCam& c = textsnap::cam;
    if (text_cam.cam_frame == c.frame)
        return;
    text_cam.cam_frame = c.frame;
    text_cam.valid = false;
    if (c.frame < 0 || c.vp_w <= 0.0f || c.vp_h <= 0.0f)
        return;
    D3DMATRIX world, view, proj;
    memcpy(&world, c.world, sizeof(world));
    memcpy(&view, c.view, sizeof(view));
    memcpy(&proj, c.proj, sizeof(proj));
    text_cam.fwd = vrmod::Multiply(vrmod::Multiply(world, view), proj);
    if (!InvertMatrix(text_cam.fwd, text_cam.inv))
        return;
    // Camera position: translation of the inverted world*view (rigid in
    // practice - the text pass uses an identity world matrix).
    const D3DMATRIX cam = vrmod::RigidInverse(vrmod::Multiply(world, view));
    text_cam.campos[0] = cam._41;
    text_cam.campos[1] = cam._42;
    text_cam.campos[2] = cam._43;
    text_cam.valid = true;
}

// Project a world point through the captured text camera to the game's
// screen coordinates, including the post-projection offset pair the
// game subtracts (see textsnap's context map).
inline bool TextCamProject(const float w[3], float& sx, float& sy) {
    const float* m = &text_cam.fwd._11;
    const float cx = w[0] * m[0] + w[1] * m[4] + w[2] * m[8] + m[12];
    const float cy = w[0] * m[1] + w[1] * m[5] + w[2] * m[9] + m[13];
    const float cw = w[0] * m[3] + w[1] * m[7] + w[2] * m[11] + m[15];
    if (cw < 1e-4f)
        return false;
    const textsnap::FrameCam& c = textsnap::cam;
    sx = c.vp_x + (cx / cw * 0.5f + 0.5f) * c.vp_w - c.off_x;
    sy = c.vp_y + (0.5f - cy / cw * 0.5f) * c.vp_h - c.off_y;
    return true;
}

// Unproject a game-screen point to the world position 'depth' game units
// from the text camera (the inverse of the game's text projection).
inline void TextCamUnproject(float sx, float sy, float depth, float out[3]) {
    const textsnap::FrameCam& c = textsnap::cam;
    const float ndc_x = ((sx + c.off_x) - c.vp_x) * 2.0f / c.vp_w - 1.0f;
    const float ndc_y = 1.0f - ((sy + c.off_y) - c.vp_y) * 2.0f / c.vp_h;
    // A far point on this pixel's ray, via the inverse transform.
    const float* m = &text_cam.inv._11;
    const float z = 0.9f;
    float px = ndc_x * m[0] + ndc_y * m[4] + z * m[8] + m[12];
    float py = ndc_x * m[1] + ndc_y * m[5] + z * m[9] + m[13];
    float pz = ndc_x * m[2] + ndc_y * m[6] + z * m[10] + m[14];
    float pw = ndc_x * m[3] + ndc_y * m[7] + z * m[11] + m[15];
    if (pw > -1e-8f && pw < 1e-8f)
        pw = 1e-8f;
    px /= pw;
    py /= pw;
    pz /= pw;
    float dx = px - text_cam.campos[0];
    float dy = py - text_cam.campos[1];
    float dz = pz - text_cam.campos[2];
    const float len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (len > 1e-6f) {
        dx /= len;
        dy /= len;
        dz /= len;
    }
    out[0] = text_cam.campos[0] + dx * depth;
    out[1] = text_cam.campos[1] + dy * depth;
    out[2] = text_cam.campos[2] + dz * depth;
}

// Classify one RHW draw as combat text (or a name label) and set its
// placement data. A draw is combat text exactly when the textsnap bracket
// is open (glyphs go through the glyph drawer 0x0082B440 inside it). Depth
// is the popup's distance from the text camera; the scaling center is the
// popup's bare projection through that camera.
inline bool IsCombatText(IDirect3DDevice9* dev, const void* data, UINT stride) {
    current_text_depth = -1.0f;
    current_text_anchor_valid = false;
    if (data == nullptr || stride < 16)
        return false;
    if (textsnap::current_snap == nullptr) {
        // A player name label: placed per eye around its world anchor.
        if (!vrmod::config.name_labels || !namelabel::Active())
            return false;
        // The label's flat anchor point comes from its first glyph, which
        // stays in 640x480 space even under a menu (namelabel.hpp).
        const float* v0 = reinterpret_cast<const float*>(data);
        if (namelabel::glyphs_in_bracket == 0) {
            namelabel::first_glyph[0] = v0[0];
            namelabel::first_glyph[1] = v0[1];
        }
        namelabel::glyphs_in_bracket++;
        current_text_target_x = namelabel::first_glyph[0] + 0.5f * namelabel::width -
                                (namelabel::icon_slot ? 16.0f : 0.0f);
        current_text_target_y = namelabel::first_glyph[1];
        current_text_depth = 1.0f;  // positive = "place this draw" (the label path measures from the eye)
        memcpy(current_text_anchor_world, namelabel::anchor_world, sizeof(current_text_anchor_world));
        current_text_anchor_valid = true;
        return true;
    }
    textsnap::RefreshCam();
    UpdateTextCam();
    if (!text_cam.valid)
        return false;
    const textsnap::Snap& s = *textsnap::current_snap;
    float ex, ey;
    if (!TextCamProject(s.world, ex, ey))
        return false;  // burst behind the game's own camera
    const float wx = s.world[0] - text_cam.campos[0];
    const float wy = s.world[1] - text_cam.campos[1];
    const float wz = s.world[2] - text_cam.campos[2];
    current_text_depth = sqrtf(wx * wx + wy * wy + wz * wz);
    current_text_target_x = ex;
    current_text_target_y = ey;
    return true;
}

inline std::vector<uint8_t> text_scratch;

// Name labels: an upright billboard around the label's world anchor,
// facing this eye. Each vertex's pixel offset from the label's flat anchor
// becomes a world offset at the anchor's distance, sized through the stock
// projection (the cullfov patch widens the live one) times text_scale.
// Returns nullptr when behind this eye.
inline const void* ProjectLabelVertices(int eye, const void* data,
                                        UINT vertex_count, UINT stride) {
    D3DMATRIX eye_view, eye_proj;
    ComputeVrEyeView(eye, eye_view);
    ComputeVrEyeProj(eye, last_persp_proj, eye_proj);
    const D3DMATRIX to_clip = vrmod::Multiply(eye_view, eye_proj);
    const float* tm = &to_clip._11;
    const D3DMATRIX& V = eye_view;
    // Row-vector view: the world direction of view +x is the first
    // column; the eye position solves view = 0.
    const float eye_right[3] = {V._11, V._21, V._31};
    const float eye_pos[3] = {
        -(V._41 * V._11 + V._42 * V._12 + V._43 * V._13),
        -(V._41 * V._21 + V._42 * V._22 + V._43 * V._23),
        -(V._41 * V._31 + V._42 * V._32 + V._43 * V._33)};
    const float* a = current_text_anchor_world;
    const float ddx = a[0] - eye_pos[0], ddy = a[1] - eye_pos[1], ddz = a[2] - eye_pos[2];
    const float dist = sqrtf(ddx * ddx + ddy * ddy + ddz * ddz);
    if (dist < 0.5f)
        return nullptr;
    // Yaw-only billboard: up is world up, so head roll never tilts the
    // label. right = worldUp x (eye -> anchor), signed to agree with the
    // eye's right; straight above/below falls back to the eye's right.
    const float up[3] = {0.0f, 1.0f, 0.0f};
    float right[3] = {ddz, 0.0f, -ddx};  // (0,1,0) x (ddx,ddy,ddz)
    const float rl = sqrtf(right[0] * right[0] + right[2] * right[2]);
    if (rl > 1e-4f) {
        right[0] /= rl;
        right[2] /= rl;
        if (right[0] * eye_right[0] + right[1] * eye_right[1] + right[2] * eye_right[2] < 0.0f) {
            right[0] = -right[0];
            right[2] = -right[2];
        }
    } else {
        right[0] = eye_right[0];
        right[1] = eye_right[1];
        right[2] = eye_right[2];
    }
    // Angular scale: layout is 640x480 in every mode, and the projection is
    // the last full-screen one (under a menu the scene projection narrows
    // for its 560x365 box).
    static float label_p11 = 0.0f, label_p22 = 0.0f;
    if (!menu_viewport_active && !GameViewportIsBoxed() && !MenuLockActive()) {
        const float scull = cullfov::applied_scale > 0.0f ? cullfov::applied_scale : 1.0f;
        label_p11 = last_persp_proj._11 / scull;
        label_p22 = last_persp_proj._22 / scull;
    }
    const float p11 = label_p11, p22 = label_p22;
    const float vw = (float)resolution::kGameWidth, vh = (float)resolution::kGameHeight;
    if (p11 == 0.0f || p22 == 0.0f || vw <= 0.0f || vh <= 0.0f)
        return nullptr;
    const float ts = vrmod::config.text_scale;
    const float kx = 2.0f * dist / (vw * p11) * ts;  // world units per flat pixel
    const float ky = 2.0f * dist / (vh * p22) * ts;
    const auto* src = static_cast<const uint8_t*>(data);
    text_scratch.assign(src, src + size_t(vertex_count) * stride);
    uint8_t* vtx = text_scratch.data();
    for (UINT i = 0; i < vertex_count; i++, vtx += stride) {
        float* v = reinterpret_cast<float*>(vtx);
        const float ox = (v[0] - current_text_target_x) * kx;
        const float oy = (v[1] - current_text_target_y) * ky;  // screen y grows DOWN
        float wp[3];
        wp[0] = a[0] + right[0] * ox - up[0] * oy;
        wp[1] = a[1] + right[1] * ox - up[1] * oy;
        wp[2] = a[2] + right[2] * ox - up[2] * oy;
        const float px = wp[0] * tm[0] + wp[1] * tm[4] + wp[2] * tm[8] + tm[12];
        const float py = wp[0] * tm[1] + wp[1] * tm[5] + wp[2] * tm[9] + tm[13];
        const float pz = wp[0] * tm[2] + wp[1] * tm[6] + wp[2] * tm[10] + tm[14];
        const float pw = wp[0] * tm[3] + wp[1] * tm[7] + wp[2] * tm[11] + tm[15];
        if (pw < 1e-3f)
            return nullptr;  // behind this eye
        v[0] = (px / pw * 0.5f + 0.5f) * eye_width + (float)(eye * eye_width);
        v[1] = (0.5f - py / pw * 0.5f) * eye_height;
        float z = pz / pw;
        if (z < 0.0f) z = 0.0f;
        if (z > 0.9999f) z = 0.9999f;
        v[2] = z;
        v[3] = 1.0f;
    }
    return text_scratch.data();
}

// Rebuild one combat-text draw's vertices for this eye: each vertex is
// unprojected through the game's text camera at the popup's depth and
// re-projected through this eye. Name labels go to ProjectLabelVertices.
// Returns nullptr when behind this eye (the call site disables the z test).
inline const void* ProjectCombatTextVertices(int eye, const void* data,
                                             UINT vertex_count, UINT stride) {
    if (current_text_depth <= 0.0f)
        return nullptr;
    if (current_text_anchor_valid)
        return ProjectLabelVertices(eye, data, vertex_count, stride);
    D3DMATRIX eye_view, eye_proj;
    ComputeVrEyeView(eye, eye_view);
    ComputeVrEyeProj(eye, last_persp_proj, eye_proj);
    const D3DMATRIX to_clip = vrmod::Multiply(eye_view, eye_proj);
    const float* tm = &to_clip._11;
    // Glyphs are laid out in screen pixels, so their angular size follows
    // the text camera's FOV, which the cullfov patch widens (text would
    // be 1/cull_fov_scale too large). Shrink the pixel layout around the
    // popup's own projected point by the patched/stock ratio (times
    // text_scale) to get the stock angular size. It must be that point:
    // around any other, that point's own offset (e.g. the leftward spawn
    // jitter) keeps the patched scale.
    float ts = vrmod::config.text_scale;
    const float scull = cullfov::applied_scale > 0.0f ? cullfov::applied_scale : 1.0f;
    const float stock_p11 = last_persp_proj._11 / scull;
    if (stock_p11 != 0.0f)
        ts *= textsnap::cam.proj[0] / stock_p11;
    const auto* snap_src = static_cast<const uint8_t*>(data);
    text_scratch.assign(snap_src, snap_src + size_t(vertex_count) * stride);
    uint8_t* vtx = text_scratch.data();
    for (UINT i = 0; i < vertex_count; i++, vtx += stride) {
        float* v = reinterpret_cast<float*>(vtx);
        const float gx = current_text_target_x + (v[0] - current_text_target_x) * ts;
        const float gy = current_text_target_y + (v[1] - current_text_target_y) * ts;
        float wp[3];
        TextCamUnproject(gx, gy, current_text_depth, wp);
        const float px = wp[0] * tm[0] + wp[1] * tm[4] + wp[2] * tm[8] + tm[12];
        const float py = wp[0] * tm[1] + wp[1] * tm[5] + wp[2] * tm[9] + tm[13];
        const float pz = wp[0] * tm[2] + wp[1] * tm[6] + wp[2] * tm[10] + tm[14];
        const float pw = wp[0] * tm[3] + wp[1] * tm[7] + wp[2] * tm[11] + tm[15];
        if (pw < 1e-3f)
            return nullptr;  // behind this eye
        // + eye half origin: RHW pixels are wide-target absolute.
        v[0] = (px / pw * 0.5f + 0.5f) * eye_width + (float)(eye * eye_width);
        v[1] = (0.5f - py / pw * 0.5f) * eye_height;
        float z = pz / pw;
        if (z < 0.0f) z = 0.0f;
        if (z > 0.9999f) z = 0.9999f;
        v[2] = z;
        v[3] = 1.0f;
    }
    return text_scratch.data();
}

// UI by caller: which game code issued a 2D draw. For the game's generic
// 2D drawers the site that asked for the quad is the drawer's own return
// address, at a fixed distance above our return slot:
//  - 0x82B440 (quad, one stack arg; device call returns to 0x82B54F):
//    five pushed args above a 0x70-byte frame -> +0x88.
//  - 0x82BB74 (the same shape, returns to 0x82BCAE): +0x88.
//  - 0x82B5D8 (the panel/HUD quad drawer, returns to 0x82B6AE): five
//    pushed registers, then five pushed args -> +0x2C. The menu frames
//    go through it with window-relative depth.
// Any other direct caller: the return address itself is the site. The
// window system (window manager, widgets, menu text and frames) lives in
// one code range; world effects (0x80xxxx) and the flare family
// (0x5007xx) do not.
// The vertex-list strip drawers (the menu frame's glow overlay uses the
// textured one) convert a caller vertex array in place, then call the
// device with five pushed args; each is reached only through a thin
// wrapper that adds one more return:
//  - 0x836D04 textured strip (device call returns to 0x836DC1): saved
//    ebp + 8 bytes of locals -> its caller at +0x24; its callers are the
//    wrappers 0x82B148 / 0x82B158 (flat frames), so the site is +0x28.
//  - 0x836C58 untextured strip (returns to 0x836CFC): same frame; its
//    wrapper 0x82B1A0 is flat -> +0x28.
//  - 0x836DC8 trianglelist (returns to 0x836E78): saved edi + ebp + 8
//    bytes -> +0x28; its wrapper 0x82B558 holds 0x18 bytes at the call
//    (0x14 locals + one pushed arg) -> +0x44.
constexpr uintptr_t QUAD_DRAWER_DEVICE_RET = 0x0082B54F;
constexpr uintptr_t QUAD_DRAWER2_DEVICE_RET = 0x0082BCAE;
constexpr uintptr_t PANEL_DRAWER_DEVICE_RET = 0x0082B6AE;
constexpr uintptr_t STRIP_DRAWER_TEX_DEVICE_RET = 0x00836DC1;
constexpr uintptr_t STRIP_DRAWER_FLAT_DEVICE_RET = 0x00836CFC;
constexpr uintptr_t TRILIST_DRAWER_DEVICE_RET = 0x00836E78;
constexpr size_t QUAD_DRAWER_RET_SLOT = 0x88 / 4;
constexpr size_t PANEL_DRAWER_RET_SLOT = 0x2C / 4;
constexpr size_t STRIP_DRAWER_RET_SLOT = 0x28 / 4;
constexpr size_t TRILIST_DRAWER_RET_SLOT = 0x44 / 4;
constexpr uintptr_t WINDOW_SYSTEM_CODE_BEGIN = 0x00700000;
// The end includes the item list's frame and quantity draws (0x760C64 /
// 0x760D02 / 0x760D9E, in window-code function 0x760B86..0x760DA9, called
// from 0x7368xx). No other 2D drawer call site lies between 0x760E00 and
// 0x79A000.
constexpr uintptr_t WINDOW_SYSTEM_CODE_END = 0x00761000;
// The exe's code section (image base 0x400000, no ASLR): a resolved site
// outside it means the stack shape assumed above did not hold.
constexpr uintptr_t EXE_CODE_BEGIN = 0x00401000;
constexpr uintptr_t EXE_CODE_END = 0x008CC000;
inline size_t DrawerSiteSlot(uintptr_t ret) {
    if (ret == QUAD_DRAWER_DEVICE_RET || ret == QUAD_DRAWER2_DEVICE_RET)
        return QUAD_DRAWER_RET_SLOT;
    if (ret == PANEL_DRAWER_DEVICE_RET)
        return PANEL_DRAWER_RET_SLOT;
    if (ret == STRIP_DRAWER_TEX_DEVICE_RET || ret == STRIP_DRAWER_FLAT_DEVICE_RET)
        return STRIP_DRAWER_RET_SLOT;
    if (ret == TRILIST_DRAWER_DEVICE_RET)
        return TRILIST_DRAWER_RET_SLOT;
    return 0;
}
inline uintptr_t DrawSiteFromReturnSlot(uintptr_t* slot) {
    const size_t k = DrawerSiteSlot(slot[0]);
    if (k == 0)
        return slot[0];
    const uintptr_t site = slot[k];
    return (site >= EXE_CODE_BEGIN && site < EXE_CODE_END) ? site : slot[0];
}
// The HUD glyph-string drawers 0x789248 / 0x789440 / 0x7895D4 (layout via
// 0x7A9044, glyph quads through 0x82B440 at sites 0x789343 / 0x78959B /
// 0x789653, then 0x7896B7) draw HUD digits and text at a fixed z 0.3333
// = 3 units, called from HUD widget code 0x77Bxxx. Alpha-blended and
// textured outside the window range, they count as UI callers so the
// alpha-sprite rule never admits them.
constexpr uintptr_t HUD_TEXT_CODE_BEGIN = 0x00789240;
constexpr uintptr_t HUD_TEXT_CODE_END = 0x007896D4;
inline bool IsWindowSystemSite(uintptr_t site) {
    return (site >= WINDOW_SYSTEM_CODE_BEGIN && site < WINDOW_SYSTEM_CODE_END) ||
           (site >= HUD_TEXT_CODE_BEGIN && site < HUD_TEXT_CODE_END);
}

// Fixed-depth HUD draws that look like alpha-blended sprites (SRCALPHA /
// INVSRCALPHA, depth test on), excluded by site: the screen fade quad
// (0x8047A4, untextured, z 0.9 = 10 units, full screen) and the
// equipped-weapon icon (0x81A29D, z 0.2 = 1 unit).
constexpr uintptr_t HUD_ALPHA_SITES[] = { 0x008047A4, 0x0081A29D };

// Alpha-blended world sprite ([vr] alpha_sprite_rule): e.g. Forest grass
// tufts (site 0x802412) and the Central Control Area fog (0x50891F),
// CPU-projected, depth-tested, textured quads blended SRCALPHA /
// INVSRCALPHA. Requires a bound texture and texture coordinates, a caller
// outside the window system and none of the sites above; IsWorldRhw
// applies the depth floor.
inline bool IsAlphaSpriteBlend(IDirect3DDevice9* dev, DWORD dest_blend, uintptr_t site) {
    if (!vrmod::config.alpha_sprite_rule)
        return false;
    if (dest_blend != D3DBLEND_INVSRCALPHA)
        return false;
    DWORD src_blend = 0, alpha_blend = 0;
    dev->GetRenderState(D3DRS_SRCBLEND, &src_blend);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &alpha_blend);
    if (src_blend != D3DBLEND_SRCALPHA || alpha_blend == 0)
        return false;
    if ((probe::current_fvf & D3DFVF_TEXCOUNT_MASK) == 0)
        return false;
    if (IsWindowSystemSite(site))
        return false;
    for (uintptr_t s : HUD_ALPHA_SITES)
        if (site == s)
            return false;
    IDirect3DBaseTexture9* texture = nullptr;
    dev->GetTexture(0, &texture);
    if (texture == nullptr)
        return false;
    texture->Release();
    return true;
}

// Set by IsWorldRhw for an alpha-blended sprite closer than [vr]
// alpha_sprite_floor: the draw is skipped, not routed to the HUD (a fog
// puff there darkens the whole HUD). The call site reads and clears it.
inline bool drop_alpha_sprite = false;

// Index of the game particle pool whose draw method is on the stack (-1 =
// none); published by the hooks in psobbvr_particlepool.hpp.
inline int current_particle_pool = -1;

inline bool IsWorldRhw(IDirect3DDevice9* dev, const void* data, UINT stride, uintptr_t site = 0) {
    drop_alpha_sprite = false;
    if (data == nullptr || stride < 16)
        return false;
    DWORD dest_blend = 0, z_enable = 0;
    dev->GetRenderState(D3DRS_DESTBLEND, &dest_blend);
    dev->GetRenderState(D3DRS_ZENABLE, &z_enable);
    if (z_enable == D3DZB_FALSE)
        return false;
    // Additive (dest ONE) is the world-effect blend; alpha-blended sprites
    // need the narrower rule above.
    const bool alpha_sprite = dest_blend != D3DBLEND_ONE;
    if (alpha_sprite && !IsAlphaSpriteBlend(dev, dest_blend, site))
        return false;
    if (alpha_sprite) {
        // Alpha sprites have their own lower floor (grass at the feet is
        // 1..4 units away; HUD look-alikes are excluded by caller above).
        // Closer than alpha_sprite_floor the draw is dropped.
        const float p33 = last_persp_proj._33, p43 = last_persp_proj._43;
        const float z = reinterpret_cast<const float*>(data)[2];
        const float denom = p33 + z;
        if (denom > -1e-6f)
            return false;
        const float zview = -p43 / denom;  // negative, game units
        if (zview < -vrmod::config.alpha_sprite_floor)
            return true;
        drop_alpha_sprite = true;
        return false;
    }
    // Depth floor ([vr] sprite_near_floor): additive
    // quads closer than this are screen-space (lens flares at 2 units, HUD
    // glows at ~3). The gun muzzle flash spawns at the hand, 5-8 units
    // from the eye, so the floor must stay below that.
    const float p33 = last_persp_proj._33, p43 = last_persp_proj._43;
    const float z = reinterpret_cast<const float*>(data)[2];
    const float denom = p33 + z;
    if (denom > -1e-6f)
        return false;
    const float zview = -p43 / denom;  // negative, game units
    return zview < -vrmod::config.sprite_near_floor;
}

inline bool EffectReprojActive() {
    // Gameplay frames only (menu screens leave misleading matrices in the
    // scene slots), plus burst-tunnel frames.
    return VrDrives() && (gamecam::DrivesView() || BurstActive()) &&
           cpu_view_valid && last_persp_proj_valid;
}

// [vr] hide_sun: the sun glow and its lens-flare ghosts are positioned from
// the game's internal camera and drift against the head-tracked view.
// Every quad of the family is additive (dest ONE, depth test on) at
// exactly z = 0.5 (view depth 2.0 units); no other draw uses that z (HUD
// glows 0.31..0.68, world effects 0.93+). Skipped while VR drives the view.
inline bool IsHiddenEffect(IDirect3DDevice9* dev, const void* data, UINT stride) {
    if (!vrmod::config.hide_sun || !VrDrives())
        return false;
    if (data == nullptr || stride < 16)
        return false;
    DWORD dest_blend = 0, z_enable = 0;
    dev->GetRenderState(D3DRS_DESTBLEND, &dest_blend);
    dev->GetRenderState(D3DRS_ZENABLE, &z_enable);
    if (dest_blend != D3DBLEND_ONE || z_enable == D3DZB_FALSE)
        return false;
    const float z = reinterpret_cast<const float*>(data)[2];
    return z > 0.4995f && z < 0.5005f;
}


inline std::vector<uint8_t> effect_scratch;

// Undo the game's CPU projection (depth from the vertex z, frustum from its
// last perspective projection, camera from cpu_view) and re-project every
// vertex through this eye's view and frustum, so the sprite sits at its
// world position in both eyes. Returns nullptr when it lands behind the
// eye (skip the draw).
inline const void* ReprojectEffectVertices(int eye, const void* data,
                                           UINT vertex_count, UINT stride) {
    const float p11 = last_persp_proj._11, p22 = last_persp_proj._22;
    const float p31 = last_persp_proj._31, p32 = last_persp_proj._32;
    const float p33 = last_persp_proj._33, p43 = last_persp_proj._43;
    if (p11 == 0.0f || p22 == 0.0f)
        return nullptr;
    // The game projected into its current viewport (640x480 space): full
    // screen, or 80,0 560x365 with the menu open.
    const D3DVIEWPORT9& gvp = resolution::last_game_viewport;
    const float half_w = gvp.Width * 0.5f, half_h = gvp.Height * 0.5f;
    if (half_w <= 0.0f || half_h <= 0.0f)
        return nullptr;
    const float center_x = gvp.X + half_w, center_y = gvp.Y + half_h;
    // During a teleport warp, unproject through the view snapshotted at
    // warp entry instead of cpu_view (see the warp block).
    const D3DMATRIX& unproject_view =
        (warp_active && gamecam::DrivesView()) ? warp_view : cpu_view;
    D3DMATRIX eye_view, eye_proj;
    ComputeVrEyeView(eye, eye_view);
    ComputeVrEyeProj(eye, last_persp_proj, eye_proj);
    // Two passes: game screen -> world for every vertex first (the
    // upright rebuild needs the quad's centre), then world -> eye clip.
    const D3DMATRIX cam = vrmod::RigidInverse(unproject_view);  // view->world
    const D3DMATRIX to_clip = vrmod::Multiply(eye_view, eye_proj);
    const float* m = &to_clip._11;

    const auto* src = static_cast<const uint8_t*>(data);
    effect_scratch.assign(src, src + size_t(vertex_count) * stride);
    uint8_t* vertex = effect_scratch.data();
    // World positions per vertex for the upright rebuild; a single quad is
    // the only candidate, so the scratch is fixed-size.
    float world_pos[4][3];
    // Only quads drawn by a listed particle pool (identity).
    bool upright_candidate = vertex_count == 4 && vrmod::config.sprite_upright &&
        vrmod::config.UprightPool(current_particle_pool);
    float sz_min = 1e30f, sz_max = -1e30f;
    for (UINT i = 0; i < vertex_count; i++, vertex += stride) {
        float* v = reinterpret_cast<float*>(vertex);
        // Off-frustum guard: the game does not cull effect sprites, so ones
        // outside its frustum (or behind its camera) arrive with wild
        // screen coordinates that the flat game clips but would unproject
        // to garbage positions. Allow half a viewport of overhang, drop
        // the draw beyond that.
        if (v[0] < gvp.X - half_w || v[0] > gvp.X + gvp.Width + half_w ||
            v[1] < gvp.Y - half_h || v[1] > gvp.Y + gvp.Height + half_h)
            return nullptr;
        // Depth from z: z_ndc = (zv*p33 + p43) / (-zv)  =>  zv = -p43/(p33+z).
        const float denom = p33 + v[2];
        if (denom > -1e-6f)  // at/behind the far-plane singularity
            return nullptr;
        const float zview = -p43 / denom;  // negative (in front), RH
        if (zview > -1e-3f)
            return nullptr;  // degenerate/behind the game camera
        const float w = -zview;
        // game screen -> game NDC -> the game camera's view space.
        const float ndc_x = (v[0] - center_x) / half_w;
        const float ndc_y = (center_y - v[1]) / half_h;
        const float xv = w * (ndc_x + p31) / p11;
        const float yv = w * (ndc_y + p32) / p22;
        const float zv = -w;
        const float wx = xv * cam._11 + yv * cam._21 + zv * cam._31 + cam._41;
        const float wy = xv * cam._12 + yv * cam._22 + zv * cam._32 + cam._42;
        const float wz = xv * cam._13 + yv * cam._23 + zv * cam._33 + cam._43;
        if (upright_candidate) {
            world_pos[i][0] = wx; world_pos[i][1] = wy; world_pos[i][2] = wz;
            if (v[2] < sz_min) sz_min = v[2];
            if (v[2] > sz_max) sz_max = v[2];
        }
        // Pass 2 reads the world position from the vertex itself (v[3],
        // the rhw slot, is rewritten there anyway).
        v[0] = wx; v[1] = wy; v[2] = wz;
    }
    // Upright rebuild ([vr] sprite_upright): the round trip keeps the quad
    // square to the head's forward, so a tall sprite (the telepipe beam)
    // leans with head pitch. Keep the centre and in-plane offsets, but
    // span them with world up and the levelled camera right: a cylindrical
    // billboard. Only true screen sprites (all four corners at one depth)
    // qualify; the bullet-trail ribbon (psobbvr_trail.hpp) has per-corner
    // depths and keeps its shape.
    if (upright_candidate && sz_max - sz_min > 1e-6f)
        upright_candidate = false;
    if (upright_candidate) {
        float rx = cam._11, rz = cam._13;  // camera right (row 1), levelled
        const float rlen = sqrtf(rx * rx + rz * rz);
        if (rlen > 1e-3f) {
            rx /= rlen; rz /= rlen;
            const float R[3] = { cam._11, cam._12, cam._13 };
            const float U[3] = { cam._21, cam._22, cam._23 };
            float c[3] = { 0, 0, 0 };
            for (int q = 0; q < 4; q++)
                for (int k = 0; k < 3; k++) c[k] += world_pos[q][k] * 0.25f;
            vertex = effect_scratch.data();
            for (int q = 0; q < 4; q++, vertex += stride) {
                const float d[3] = { world_pos[q][0] - c[0], world_pos[q][1] - c[1], world_pos[q][2] - c[2] };
                const float a = d[0] * R[0] + d[1] * R[1] + d[2] * R[2];  // along camera right
                const float b = d[0] * U[0] + d[1] * U[1] + d[2] * U[2];  // along camera up
                float* v = reinterpret_cast<float*>(vertex);
                v[0] = c[0] + a * rx;
                v[1] = c[1] + b;
                v[2] = c[2] + a * rz;
            }
        }
    }
    // Pass 2: world -> this eye's clip -> wide-target pixels.
    vertex = effect_scratch.data();
    for (UINT i = 0; i < vertex_count; i++, vertex += stride) {
        float* v = reinterpret_cast<float*>(vertex);
        const float wx = v[0], wy = v[1], wz = v[2];
        const float cx = wx * m[0] + wy * m[4] + wz * m[8] + m[12];
        const float cy = wx * m[1] + wy * m[5] + wz * m[9] + m[13];
        const float cz = wx * m[2] + wy * m[6] + wz * m[10] + m[14];
        const float cw = wx * m[3] + wy * m[7] + wz * m[11] + m[15];
        if (cw < 1e-3f)
            return nullptr;  // behind the eye - drop the sprite this frame
        float z = cz / cw;
        if (z < 0.0f) z = 0.0f;
        if (z > 0.9999f) z = 0.9999f;  // keep extreme-range sprites drawable
        // + eye half origin: RHW pixels are wide-target absolute.
        v[0] = (cx / cw * 0.5f + 0.5f) * eye_width + (float)(eye * eye_width);
        v[1] = (0.5f - cy / cw * 0.5f) * eye_height;
        v[2] = z;
        v[3] = 1.0f / cw;
    }
    return effect_scratch.data();
}

// Issue one draw/clear into both eye halves. Leaves the right eye's
// viewport and matrices set; the next duplicated call selects its own. The
// callable receives the eye index (per-eye call sites use it).
//
// hud marks screen-space UI draws (RHW): identical UI in both eyes does not
// fuse under the asymmetric VR frustums, so with VR it goes on the virtual
// screen: both eyes, the call site remapping the vertices per eye.
// hud_remappable = the call site can (user-pointer draws only); a
// non-remappable draw goes to the left eye only.
template <typename CallFn>
inline void Duplicate(IDirect3DDevice9* dev, CallFn&& call, bool hud = false,
                      bool hud_remappable = false, bool world_rhw = false) {
    if (!EnsureTargets(dev))
        return call(0);
    if (world_rhw && VrDrives()) {
        // CPU-projected world sprite, re-projected per eye at the call
        // site: clip to the eye's full half with the game's depth range.
        for (int eye = 0; eye < 2; eye++) {
            BeginEye(dev, eye);
            D3DVIEWPORT9 full = { (DWORD)(eye * eye_width), 0,
                                  eye_width, eye_height, 0.0f, 1.0f };
            if (current_viewport.Width != 0) {
                full.MinZ = current_viewport.MinZ;
                full.MaxZ = current_viewport.MaxZ;
            }
            dev->SetViewport(&full);
            call(eye);
        }
        return;
    }
    if (hud && VrDrives()) {
        if (hud_remappable) {
            for (int eye = 0; eye < 2; eye++) {
                BeginEye(dev, eye);
                // Remapped UI can leave the game's viewport: clip to the
                // eye's full half with the game's depth range.
                D3DVIEWPORT9 full = { (DWORD)(eye * eye_width), 0,
                                      eye_width, eye_height, 0.0f, 1.0f };
                if (current_viewport.Width != 0) {
                    full.MinZ = current_viewport.MinZ;
                    full.MaxZ = current_viewport.MaxZ;
                }
                dev->SetViewport(&full);
                call(eye);
            }
            return;
        }
        BeginEye(dev, 0);  // non-remappable: left eye only, never doubled
        call(0);
        return;
    }
    // Developer-build perf diagnostic: 1 = left eye only; other values
    // render both.
    if (vrmod::config.one_eye_debug == 1) {
        BeginEye(dev, 0);
        call(0);
        return;
    }
    for (int eye = 0; eye < 2; eye++) {
        BeginEye(dev, eye);
        call(eye);
    }
}

// Squeeze both eye images side by side onto the real backbuffer and restore
// the backbuffer + game depth-stencil as the render target so later draws
// (the overlay panel) land on the visible frame.
inline void FinishFrame() {
    if ((!enabled && !double_wide) || composited || device == nullptr || wide_rt == nullptr)
        return;
    composited = true;  // even on partial failure, don't retry this frame

    IDirect3DSurface9* backbuffer = nullptr;
    const HRESULT hr_bb = device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
    if (FAILED(hr_bb)) {
        probe::Log("stereo: composite GetBackBuffer FAILED 0x%08X", hr_bb);
        return;
    }
    D3DSURFACE_DESC bb_desc;
    backbuffer->GetDesc(&bb_desc);

    const HRESULT hr_rt = device->SetRenderTarget(0, backbuffer);
    device->SetDepthStencilSurface(game_ds);
    wide_bound = false;  // the backbuffer is bound now

    const LONG half = (LONG)bb_desc.Width / 2;
    RECT left_half = { 0, 0, half, (LONG)bb_desc.Height };
    RECT right_half = { half, 0, (LONG)bb_desc.Width, (LONG)bb_desc.Height };
    // Both eyes live in the wide target's halves; cross-eye layout puts the
    // LEFT eye's image on the RIGHT half of the window.
    RECT eye_src[2] = { { 0, 0, (LONG)eye_width, (LONG)eye_height },
                        { (LONG)eye_width, 0, (LONG)(eye_width * 2), (LONG)eye_height } };
    const RECT& left_src = eye_src[cross_eye ? 1 : 0];
    const RECT& right_src = eye_src[cross_eye ? 0 : 1];
    const HRESULT hr_l = device->StretchRect(wide_rt, &left_src, backbuffer, &left_half, D3DTEXF_LINEAR);
    const HRESULT hr_r = device->StretchRect(wide_rt, &right_src, backbuffer, &right_half, D3DTEXF_LINEAR);

    static bool logged_first = false;
    if (!logged_first || FAILED(hr_l) || FAILED(hr_r)) {
        logged_first = true;
        probe::Log("stereo: composite bb=%ux%u rebind=0x%08X stretch L=0x%08X R=0x%08X",
                   bb_desc.Width, bb_desc.Height, hr_rt, hr_l, hr_r);
    }

    // Probe mode: every 120 frames log two mid-row pixels of the eye target
    // and of the backbuffer, to tell missing draws from a failed composite.
    static int readback_countdown = 0;
    if (probe::enabled && readback_countdown++ % 120 == 0) {
        auto sample = [&](IDirect3DSurface9* surface, const char* name) {
            D3DSURFACE_DESC desc;
            surface->GetDesc(&desc);
            IDirect3DSurface9* sys = nullptr;
            if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                                           D3DPOOL_SYSTEMMEM, &sys, nullptr)))
                return;
            if (SUCCEEDED(device->GetRenderTargetData(surface, sys))) {
                D3DLOCKED_RECT locked;
                if (SUCCEEDED(sys->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
                    const BYTE* mid_row = static_cast<const BYTE*>(locked.pBits) + locked.Pitch * (desc.Height / 2);
                    const DWORD left_px = *reinterpret_cast<const DWORD*>(mid_row + 4 * (desc.Width / 4));
                    const DWORD right_px = *reinterpret_cast<const DWORD*>(mid_row + 4 * (3 * desc.Width / 4));
                    probe::Log("stereo: %s %ux%u mid-row pixels: quarter=0x%08X three-quarter=0x%08X",
                               name, desc.Width, desc.Height, left_px, right_px);
                    sys->UnlockRect();
                }
            }
            sys->Release();
        };
        // On the wide target the quarter / three-quarter pixels are the
        // middle of each eye.
        sample(wide_rt, "wide-eyes");
        sample(backbuffer, "backbuffer");
    }

    backbuffer->Release();
}

// Called after Present was forwarded: the next game calls start a new frame.
inline void OnFrameEnd() {
    composited = false;
    paused = false;
    // Fresh WaitPoses pose (and possibly a fresh menu anchor) next frame.
    InvalidateEyeCache();
    // Menu-open viewport latch (see NoteGameViewport).
    menu_viewport_active = menu_viewport_seen;
    menu_viewport_seen = false;
    // Burst latch (see the burst block): counted per present across both
    // render passes, with hysteresis for the tunnel's sparse tail.
    const bool burst_next = burst_candidates >= (burst_active ? 16 : 48);
    if (burst_next != burst_active)
        diag::Log("burst3d: latch %s (candidates=%d)",
                  burst_next ? "ON" : "off", burst_candidates);
    burst_active = burst_next;
    burst_candidates = 0;
    // Teleport-warp latch (see the warp block). DrivesView still reports
    // the ended frame here (gamecam::Apply resets it next frame).
    const bool warp_next = vrmod::config.burst_3d && gamecam::DrivesView() &&
                           warp_candidates >= (warp_active ? 32 : 100) &&
                           scene_persp_draws <= (warp_active ? 50 : 20);
    if (warp_next && !warp_active) {
        // Snapshot the head view at warp entry (not game_view, which by
        // frame end holds whatever a UI pass set last).
        D3DMATRIX head;
        gamecam::GetWorldFromHead(head);
        warp_view = vrmod::RigidInverse(head);
    }
    if (warp_next != warp_active)
        diag::Log("burst3d: warp latch %s (sprites=%d scene=%d)",
                  warp_next ? "ON" : "off", warp_candidates, scene_persp_draws);
    warp_active = warp_next;
    warp_candidates = 0;
    scene_persp_draws = 0;
    // Scene view/projection must be re-seen (from their slots) each frame.
    cpu_view_valid = false;
    last_persp_proj_valid = false;
    // HUD texture: fresh clear + fresh content next frame.
    hud_cleared = false;
    hud_drawn = false;
    // Menu world-lock state likewise.
    menu_below_cleared = false;
    menu_below_drawn = false;
    menu_seen_3d = false;
    menu_below_bound = false;
    menu_above_rects.clear();
    menu_float_prev.swap(menu_float_accum);
    menu_float_accum.clear();
    // Merge this frame's accumulated 3D bounds into the rolling envelope
    // and rebuild the routing reference from it (see menu_3d_hold).
    if (menu_3d_accum_full)
        menu_3d_hold_full[0] = true;
    if (menu_3d_accum_valid) {
        if (!menu_3d_hold_valid[0]) {
            menu_3d_hold[0] = menu_3d_accum;
            menu_3d_hold_valid[0] = true;
        } else {
            if (menu_3d_accum.x0 < menu_3d_hold[0].x0) menu_3d_hold[0].x0 = menu_3d_accum.x0;
            if (menu_3d_accum.y0 < menu_3d_hold[0].y0) menu_3d_hold[0].y0 = menu_3d_accum.y0;
            if (menu_3d_accum.x1 > menu_3d_hold[0].x1) menu_3d_hold[0].x1 = menu_3d_accum.x1;
            if (menu_3d_accum.y1 > menu_3d_hold[0].y1) menu_3d_hold[0].y1 = menu_3d_accum.y1;
        }
    }
    if (++menu_3d_hold_frames >= 30) {
        menu_3d_hold_frames = 0;
        menu_3d_hold[1] = menu_3d_hold[0];
        menu_3d_hold_valid[1] = menu_3d_hold_valid[0];
        menu_3d_hold_full[1] = menu_3d_hold_full[0];
        menu_3d_hold_valid[0] = false;
        menu_3d_hold_full[0] = false;
    }
    menu_3d_ref_full = menu_3d_hold_full[0] || menu_3d_hold_full[1];
    menu_3d_ref_valid = menu_3d_hold_valid[0] || menu_3d_hold_valid[1];
    if (menu_3d_ref_valid) {
        const MenuRect* a = menu_3d_hold_valid[0] ? &menu_3d_hold[0] : &menu_3d_hold[1];
        menu_3d_ref = *a;
        if (menu_3d_hold_valid[0] && menu_3d_hold_valid[1]) {
            if (menu_3d_hold[1].x0 < menu_3d_ref.x0) menu_3d_ref.x0 = menu_3d_hold[1].x0;
            if (menu_3d_hold[1].y0 < menu_3d_ref.y0) menu_3d_ref.y0 = menu_3d_hold[1].y0;
            if (menu_3d_hold[1].x1 > menu_3d_ref.x1) menu_3d_ref.x1 = menu_3d_hold[1].x1;
            if (menu_3d_hold[1].y1 > menu_3d_ref.y1) menu_3d_ref.y1 = menu_3d_hold[1].y1;
        }
        menu_3d_ref.x0 -= 6.0f;
        menu_3d_ref.y0 -= 6.0f;
        menu_3d_ref.x1 += 6.0f;
        menu_3d_ref.y1 += 6.0f;
    }
    menu_3d_accum_valid = false;
    menu_3d_accum_full = false;
    // Combat-text snapshots: advance the frame counter, expire stale entries.
    textsnap::OnFrameEnd();
}

// Wait for the GPU to finish the eye targets before the VR backend submits
// them from D3D11 (shared D3D9 surfaces have no cross-API sync). Event
// query with a bounded 50 ms spin.
inline void FlushForVr() {
    if (device == nullptr)
        return;
    if (flush_query == nullptr &&
        FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT, &flush_query)))
        return;
    flush_query->Issue(D3DISSUE_END);
    const DWORD deadline = GetTickCount() + 50;
    while (flush_query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
        if (GetTickCount() > deadline) {
            probe::Log("stereo: VR flush timed out");
            break;
        }
        Sleep(0);
    }
}

// The per-frame VR tail, called from Present after the composite: flush the
// eye targets, submit them, then block for the next frame's head pose.
inline void VrEndOfFrame() {
    if (!VrDrives() || wide_rt == nullptr)
        return;
    FlushForVr();
    // Hide the HUD quad on frames that drew nothing into it.
    vrmod::Get()->SetHudLayerVisible(hud_drawn && HudLayerActive());
    // Tell the backend which menu quads have fresh content, and drop the
    // menu anchor when gameplay starts so the next menu re-captures it.
    static bool was_driving = false;
    const bool driving = gamecam::DrivesView();
    if (driving && !was_driving)
        vrmod::Get()->InvalidateMenuAnchor();
    was_driving = driving;
    vrmod::Get()->SetMenuLayerState(menu_below_drawn && MenuLockActive(),
                                    hud_drawn && MenuLockActive());
    vrmod::Get()->SubmitFrame();
    vrmod::Get()->WaitPoses();
}

} // namespace stereo
