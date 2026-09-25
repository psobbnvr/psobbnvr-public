#pragma once

// On-screen keyboard ([vr] vr_keyboard): a panel below eye level whenever
// the game has a text field open (chat, login, name entry...), driven by
// either controller's aim ray and trigger.
//
// Detection: every text field is one edit-box class whose tick calls
// EDIT_OPEN_ADDR while the field is open (bit 0 of +EDIT_FLAGS_OFFSET)
// and 0x72F37C while closed. A detour on the open routine stamps the
// frame it last ran; the field counts as open while the stamp is at most
// 3 frames old. The same object gives the preview line: its text
// (+EDIT_TEXT_OFFSET) and cursor (+EDIT_CURSOR_OFFSET).
//
// Typing: printable characters reach the game only as WM_CHAR messages
// (the window procedure fills a one-character slot the edit box reads),
// so one WM_CHAR is posted per frame. Enter / Backspace are ignored there
// and go through the synthetic DirectInput keys instead.
//
// Input: while the panel is up the controller belongs to it. A trigger
// pull on a key presses it; off the panel it is Enter (right hand) or
// Backspace (left). Shift is one-shot, Caps latches. Enter closes the
// field and the panel follows. Each hand's ray is drawn as a beam
// (VRInterface::SetPanelRay).
//
// Placement: fixed when the panel opens - keyboard_distance_m ahead of
// the head, keyboard_drop_m below eye level, tilted to face the head.
// OpenXR backend only (VRInterface::ShowPanel).

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "MinHook.h"
#include "psobbvr_log.hpp"
#include "psobbvr_probe.hpp"
#include "psobbvr_stereo.hpp"
#include "psobbvr_vr.hpp"

namespace vrkeyboard {

// The edit box's open routine: its tick (0x72EE54, vtable 0xB40548 slot
// 1) calls this while bit 0 of the flags is set (focused, drawn, taking
// characters). `this` in ECX, no stack arguments.
constexpr uintptr_t EDIT_OPEN_ADDR = 0x0072FDD0;
constexpr uintptr_t EDIT_TEXT_OFFSET = 0x2EC;    // wchar_t[0x80], NUL-terminated
constexpr uintptr_t EDIT_CURSOR_OFFSET = 0x3EC;  // int, index into the text
constexpr uintptr_t EDIT_FLAGS_OFFSET = 0x3F0;   // bit 0 = the field is open
constexpr uint32_t EDIT_FLAG_OPEN = 1u;
// "Game actions enabled": the open routine writes 0 each tick, the closed
// routine and the destructor write 1, and the action reader (0x78FD68)
// ignores key / pad actions while it is 0. Logged as a cross-check.
constexpr uintptr_t ACTIONS_ENABLED_ADDR = 0x009D09E8;
// The UI-focus word (psobbvr_controller.hpp GameUiFocused), logged too.
constexpr uintptr_t UI_FOCUS_BASE_ADDR = 0x00A98478;

// DIK scancodes for the control keys (the key-state route).
constexpr BYTE SC_BACKSPACE = 0x0E, SC_TAB = 0x0F, SC_ENTER = 0x1C;
constexpr int KEY_PRESS_FRAMES = 3;

// ---- detection -------------------------------------------------------

using EditTickFn = void(__fastcall*)(uintptr_t self, uintptr_t edx);
inline EditTickFn original_tick = nullptr;
inline bool installed = false;
inline unsigned frame = 0;             // controller::OnFrame count
inline uintptr_t edit_obj = 0;         // the last edit box that ticked
inline unsigned edit_seen_frame = 0;   // ...and the frame it did
inline bool edit_seen_valid = false;

inline void __fastcall EditTickDetour(uintptr_t self, uintptr_t edx) {
    edit_obj = self;
    edit_seen_frame = frame;
    edit_seen_valid = true;
    original_tick(self, edx);
}

// Called from the first BeginScene after gunfire::Install (which
// initializes MinHook). Always installed; gated per frame.
inline void Install() {
    if (installed)
        return;
    installed = true;
    if (MH_CreateHook(reinterpret_cast<void*>(EDIT_OPEN_ADDR),
                      reinterpret_cast<void*>(&EditTickDetour),
                      reinterpret_cast<void**>(&original_tick)) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(EDIT_OPEN_ADDR)) != MH_OK) {
        probe::Log("vrkeyboard: edit-box tick hook failed");
        diag::Log("vrkeyboard: edit-box tick hook failed - no text-field detection");
        original_tick = nullptr;
        return;
    }
    diag::Log("vrkeyboard: edit-box open-routine hook installed at 0x%08X",
              (unsigned)EDIT_OPEN_ADDR);
}

// The state-change log's cross-checks, each -1 when unreadable.
inline int BoxOpenBit() {
    if (edit_obj == 0 || !diag::Accessible(edit_obj + EDIT_FLAGS_OFFSET, 4, false))
        return -1;
    return (*reinterpret_cast<const uint32_t*>(edit_obj + EDIT_FLAGS_OFFSET) &
            EDIT_FLAG_OPEN) ? 1 : 0;
}

inline int ActionsEnabled() {
    if (!diag::Accessible(ACTIONS_ENABLED_ADDR, 4, false))
        return -1;
    return *reinterpret_cast<const int*>(ACTIONS_ENABLED_ADDR) != 0 ? 1 : 0;
}

inline int UiFocusWord() {
    if (!diag::Accessible(UI_FOCUS_BASE_ADDR, 4, false))
        return -1;
    const uintptr_t a = *reinterpret_cast<const uintptr_t*>(UI_FOCUS_BASE_ADDR);
    if (a == 0 || !diag::Accessible(a + 0x10, 4, false))
        return -1;
    const uintptr_t b = *reinterpret_cast<const uintptr_t*>(a + 0x10);
    if (b == 0 || !diag::Accessible(b + 0x1E, 2, false))
        return -1;
    return *reinterpret_cast<const uint16_t*>(b + 0x1E);
}

// ---- layout ----------------------------------------------------------

enum KeyKind : int {
    KK_CHAR = 0, KK_BACKSPACE, KK_ENTER, KK_SHIFT, KK_CAPS, KK_SPACE
};

struct KeyDef {
    char ch;          // unshifted character (KK_CHAR)
    char shifted;     // shifted character (KK_CHAR)
    KeyKind kind;
    float units;      // width in key units
    const char* label;  // for the special keys
};

constexpr int MAX_KEYS = 64;
inline KeyDef keys[MAX_KEYS];
inline RECT key_rects[MAX_KEYS];
inline int key_count = 0;
inline bool layout_built = false;

constexpr UINT PANEL_W = 1024, PANEL_H = 400;
constexpr int TEXT_LINE_H = 56;
constexpr int ROW_H = 64;
constexpr int PAD = 10;
constexpr int GAP = 6;

inline void AddRow(int row, float indent_units, const char* plain,
                   const char* shifted, const KeyDef* before, int nbefore,
                   const KeyDef* after, int nafter) {
    const float unit = (float)(PANEL_W - 2 * PAD) / 15.0f;
    float x = PAD + indent_units * unit;
    const int y0 = TEXT_LINE_H + PAD + row * ROW_H;
    auto place = [&](const KeyDef& d) {
        if (key_count >= MAX_KEYS)
            return;
        keys[key_count] = d;
        RECT& r = key_rects[key_count];
        r.left = (LONG)(x + GAP / 2);
        r.right = (LONG)(x + d.units * unit - GAP / 2);
        r.top = y0 + GAP / 2;
        r.bottom = y0 + ROW_H - GAP / 2;
        x += d.units * unit;
        key_count++;
    };
    for (int i = 0; i < nbefore; i++)
        place(before[i]);
    for (int i = 0; plain[i] != 0; i++) {
        KeyDef d = {plain[i], shifted[i], KK_CHAR, 1.0f, nullptr};
        place(d);
    }
    for (int i = 0; i < nafter; i++)
        place(after[i]);
}

inline void BuildLayout() {
    if (layout_built)
        return;
    layout_built = true;
    key_count = 0;
    const KeyDef backspace = {0, 0, KK_BACKSPACE, 2.0f, "Backspace"};
    const KeyDef caps = {0, 0, KK_CAPS, 1.75f, "Caps"};
    const KeyDef enter = {0, 0, KK_ENTER, 2.25f, "Enter"};
    const KeyDef shift = {0, 0, KK_SHIFT, 2.25f, "Shift"};
    const KeyDef space = {0, 0, KK_SPACE, 7.0f, "Space"};
    AddRow(0, 0.0f, "`1234567890-=", "~!@#$%^&*()_+", nullptr, 0, &backspace, 1);
    AddRow(1, 0.5f, "qwertyuiop[]\\", "QWERTYUIOP{}|", nullptr, 0, nullptr, 0);
    AddRow(2, 0.0f, "asdfghjkl;'", "ASDFGHJKL:\"", &caps, 1, &enter, 1);
    AddRow(3, 0.0f, "zxcvbnm,./", "ZXCVBNM<>?", &shift, 1, nullptr, 0);
    AddRow(4, 4.0f, "", "", &space, 1, nullptr, 0);
}

inline int KeyAt(int px, int py) {
    for (int i = 0; i < key_count; i++) {
        const RECT& r = key_rects[i];
        if (px >= r.left && px < r.right && py >= r.top && py < r.bottom)
            return i;
    }
    return -1;
}

// ---- state -----------------------------------------------------------

inline bool open = false;
inline bool prev_seen = false;
inline int prev_open_bit = -2, prev_actions = -2, prev_focus = -2;  // trace
inline D3DMATRIX pose_m = {};          // the panel's tracking-space pose, meters
inline bool shift = false, caps = false;
inline int hover[2] = {-1, -1};
inline int flash_key = -1;             // the key lit by the last press
inline unsigned flash_until = 0;
inline bool prev_trig[2] = {};
inline bool dirty = true;
inline BYTE key_sc = 0;                // scancode being pressed (control keys)
inline int key_frames = 0;
inline char pending[128];              // characters queued for WM_CHAR
inline int pending_head = 0, pending_tail = 0;
inline HWND game_window = nullptr;
inline wchar_t preview[0x90] = {};     // the field's text with a caret
inline void* dib_bits = nullptr;
inline HBITMAP dib = nullptr;
inline HDC dib_dc = nullptr;
inline HFONT key_font = nullptr, text_font = nullptr;

inline bool Active() { return open; }

inline void QueueChar(char c) {
    const int next = (pending_tail + 1) % (int)sizeof(pending);
    if (next == pending_head)
        return;  // full
    pending[pending_tail] = c;
    pending_tail = next;
}

inline void PressScancode(BYTE sc) {
    key_sc = sc;
    key_frames = KEY_PRESS_FRAMES;
}

inline HWND GameWindow() {
    if (game_window == nullptr && stereo::device != nullptr) {
        D3DDEVICE_CREATION_PARAMETERS cp = {};
        if (SUCCEEDED(stereo::device->GetCreationParameters(&cp)))
            game_window = cp.hFocusWindow;
    }
    return game_window;
}

// One queued character per frame: the game's slot holds only one.
inline void PostPendingChar() {
    if (pending_head == pending_tail)
        return;
    HWND w = GameWindow();
    if (w == nullptr)
        return;
    const unsigned char c = (unsigned char)pending[pending_head];
    pending_head = (pending_head + 1) % (int)sizeof(pending);
    PostMessageA(w, WM_CHAR, (WPARAM)c, 0);
}

// ---- the field's text (preview line) ---------------------------------

inline bool ReadPreview() {
    wchar_t next[0x90] = {};
    if (edit_obj != 0 &&
        diag::Accessible(edit_obj + EDIT_TEXT_OFFSET, 0x80 * 2, false) &&
        diag::Accessible(edit_obj + EDIT_CURSOR_OFFSET, 4, false)) {
        const wchar_t* text =
            reinterpret_cast<const wchar_t*>(edit_obj + EDIT_TEXT_OFFSET);
        int cursor = *reinterpret_cast<const int*>(edit_obj + EDIT_CURSOR_OFFSET);
        int n = 0;
        while (n < 0x7F && text[n] != 0)
            n++;
        if (cursor < 0) cursor = 0;
        if (cursor > n) cursor = n;
        int o = 0;
        for (int i = 0; i < n; i++) {
            if (i == cursor)
                next[o++] = L'|';
            next[o++] = text[i];
        }
        if (cursor == n)
            next[o++] = L'|';
        next[o] = 0;
    } else {
        next[0] = 0;
    }
    if (wcscmp(next, preview) != 0) {
        wcscpy_s(preview, next);
        return true;
    }
    return false;
}

// ---- rasterization ---------------------------------------------------

inline bool EnsureDib() {
    if (dib != nullptr)
        return true;
    dib_dc = CreateCompatibleDC(nullptr);
    if (dib_dc == nullptr)
        return false;
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = (LONG)PANEL_W;
    bmi.bmiHeader.biHeight = -(LONG)PANEL_H;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    dib = CreateDIBSection(dib_dc, &bmi, DIB_RGB_COLORS, &dib_bits, nullptr, 0);
    if (dib == nullptr || dib_bits == nullptr) {
        DeleteDC(dib_dc);
        dib_dc = nullptr;
        dib = nullptr;
        return false;
    }
    SelectObject(dib_dc, dib);
    key_font = CreateFontA(-28, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    text_font = CreateFontA(-32, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            DEFAULT_PITCH | FF_SWISS, "Segoe UI");
    return true;
}

inline void Rasterize() {
    if (!EnsureDib())
        return;
    BuildLayout();
    HDC dc = dib_dc;
    // The panel: a dark slab (straight colour; the compositor multiplies
    // by the alpha set below).
    RECT all = {0, 0, (LONG)PANEL_W, (LONG)PANEL_H};
    HBRUSH bg = CreateSolidBrush(RGB(26, 30, 42));
    FillRect(dc, &all, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    // The field's text.
    HGDIOBJ old_font = SelectObject(dc, text_font);
    RECT line = {PAD + 8, 8, (LONG)PANEL_W - PAD - 8, TEXT_LINE_H};
    HBRUSH line_bg = CreateSolidBrush(RGB(14, 16, 24));
    FillRect(dc, &line, line_bg);
    DeleteObject(line_bg);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT line_text = {line.left + 10, line.top, line.right - 10, line.bottom};
    DrawTextW(dc, preview, -1, &line_text,
              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    // The keys.
    SelectObject(dc, key_font);
    const bool upper = shift != caps;
    for (int i = 0; i < key_count; i++) {
        const KeyDef& d = keys[i];
        const RECT& r = key_rects[i];
        const bool hovered = hover[0] == i || hover[1] == i;
        const bool flashing = flash_key == i && frame < flash_until;
        const bool latched = (d.kind == KK_SHIFT && shift) ||
                             (d.kind == KK_CAPS && caps);
        COLORREF fill = RGB(58, 64, 84);
        COLORREF ink = RGB(255, 255, 255);
        if (latched)
            fill = RGB(70, 120, 200);
        if (hovered) {
            fill = RGB(235, 238, 245);
            ink = RGB(20, 22, 30);
        }
        if (flashing) {
            fill = RGB(120, 200, 255);
            ink = RGB(20, 22, 30);
        }
        HBRUSH b = CreateSolidBrush(fill);
        RECT rr = r;
        FillRect(dc, &rr, b);
        DeleteObject(b);
        SetTextColor(dc, ink);
        char label[16];
        if (d.kind == KK_CHAR) {
            label[0] = upper ? d.shifted : d.ch;
            label[1] = 0;
        } else {
            strncpy_s(label, d.label, _TRUNCATE);
        }
        DrawTextA(dc, label, -1, &rr,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    SelectObject(dc, old_font);
    GdiFlush();
    // Alpha: the slab is 90% opaque everywhere (GDI leaves the byte 0).
    uint8_t* px = (uint8_t*)dib_bits;
    for (UINT i = 0; i < PANEL_W * PANEL_H; i++, px += 4)
        px[3] = 230;
}

// ---- placement + hit test --------------------------------------------

// Capture the panel pose from the head: distance ahead along the head's
// level forward, dropped below eye level, tilted to face the head. Row-
// vector matrix: rows = the panel's x / y / z axes in tracking space.
inline bool CapturePose() {
    D3DMATRIX head;
    if (!vrmod::Get()->GetHeadPose(head))
        return false;
    const float ws = vrmod::config.world_scale > 0.0f ? vrmod::config.world_scale : 1.0f;
    const float hx = head._41 / ws, hy = head._42 / ws, hz = head._43 / ws;
    float fx = -head._31, fz = -head._33;
    const float fn = sqrtf(fx * fx + fz * fz);
    if (fn > 0.2f) {
        fx /= fn;
        fz /= fn;
    } else {
        fx = 0.0f;
        fz = -1.0f;
    }
    const float d = vrmod::config.keyboard_distance_m;
    const float px = hx + fx * d;
    const float py = hy - vrmod::config.keyboard_drop_m;
    const float pz = hz + fz * d;
    // z axis: from the panel to the head (the quad's +z faces the viewer).
    float zx = hx - px, zy = hy - py, zz = hz - pz;
    const float zn = sqrtf(zx * zx + zy * zy + zz * zz);
    if (zn < 1e-4f)
        return false;
    zx /= zn; zy /= zn; zz /= zn;
    // x axis = up x z (level), y axis = z x x.
    float xx = zz, xy = 0.0f, xz = -zx;
    const float xn = sqrtf(xx * xx + xz * xz);
    if (xn < 1e-4f)
        return false;
    xx /= xn; xz /= xn;
    const float yx = zy * xz - zz * xy;
    const float yy = zz * xx - zx * xz;
    const float yz = zx * xy - zy * xx;
    pose_m = vrmod::Identity();
    pose_m._11 = xx; pose_m._12 = xy; pose_m._13 = xz;
    pose_m._21 = yx; pose_m._22 = yy; pose_m._23 = yz;
    pose_m._31 = zx; pose_m._32 = zy; pose_m._33 = zz;
    pose_m._41 = px; pose_m._42 = py; pose_m._43 = pz;
    return true;
}

// One hand's aim ray in tracking space: origin (meters) and unit
// direction.
struct AimRay {
    float ox, oy, oz, dx, dy, dz;
};

inline bool GetAimRay(int hand, AimRay& r) {
    D3DMATRIX aim;
    if (!vrmod::Get()->GetHandAimPose(hand, aim))
        return false;
    const float ws = vrmod::config.world_scale > 0.0f ? vrmod::config.world_scale : 1.0f;
    r.ox = aim._41 / ws;
    r.oy = aim._42 / ws;
    r.oz = aim._43 / ws;
    r.dx = -aim._31;
    r.dy = -aim._32;
    r.dz = -aim._33;
    return true;
}

// The visible beam's length when the ray never meets the panel's
// plane, and its width.
constexpr float RAY_FREE_LENGTH_M = 1.0f;
constexpr float RAY_WIDTH_M = 0.006f;

// The key under the ray, or -1. on_panel = the ray hit the panel at all;
// length_m = the distance to the panel's plane (RAY_FREE_LENGTH_M if it
// misses), used as the beam's length.
inline int HitKey(const AimRay& r, bool& on_panel, float& length_m) {
    on_panel = false;
    length_m = RAY_FREE_LENGTH_M;
    const float ox = r.ox, oy = r.oy, oz = r.oz;
    const float dx = r.dx, dy = r.dy, dz = r.dz;
    const D3DMATRIX inv = vrmod::RigidInverse(pose_m);
    // Into the panel's frame (row-vector).
    const float lx = ox * inv._11 + oy * inv._21 + oz * inv._31 + inv._41;
    const float ly = ox * inv._12 + oy * inv._22 + oz * inv._32 + inv._42;
    const float lz = ox * inv._13 + oy * inv._23 + oz * inv._33 + inv._43;
    const float vx = dx * inv._11 + dy * inv._21 + dz * inv._31;
    const float vy = dx * inv._12 + dy * inv._22 + dz * inv._32;
    const float vz = dx * inv._13 + dy * inv._23 + dz * inv._33;
    if (lz <= 0.0f || vz >= -1e-4f)
        return -1;  // behind the panel or pointing away
    const float t = lz / -vz;
    if (t > 3.0f)
        return -1;
    length_m = t;
    const float hx = lx + vx * t, hy = ly + vy * t;
    const float w = vrmod::config.keyboard_width_m;
    const float h = w * (float)PANEL_H / (float)PANEL_W;
    const float u = hx / w + 0.5f, v = 0.5f - hy / h;
    if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f)
        return -1;
    on_panel = true;
    return KeyAt((int)(u * PANEL_W), (int)(v * PANEL_H));
}

// The beam for one hand: a thin quad along the ray, turned about it to
// face the head (edge-on it would vanish). Frame for SetPanelRay: y from
// the tip back to the hand, z toward the head, x = y cross z.
inline void HideRay(int hand) {
    vrmod::Get()->SetPanelRay(hand, false, vrmod::Identity(), 0.0f, 0.0f);
}

inline void ShowRay(int hand, const AimRay& r, float length_m) {
    D3DMATRIX head;
    if (!vrmod::Get()->GetHeadPose(head)) {
        HideRay(hand);
        return;
    }
    const float ws = vrmod::config.world_scale > 0.0f ? vrmod::config.world_scale : 1.0f;
    const float hx = head._41 / ws, hy = head._42 / ws, hz = head._43 / ws;
    const float cx = r.ox + r.dx * length_m * 0.5f;
    const float cy = r.oy + r.dy * length_m * 0.5f;
    const float cz = r.oz + r.dz * length_m * 0.5f;
    const float yx = -r.dx, yy = -r.dy, yz = -r.dz;
    float zx = hx - cx, zy = hy - cy, zz = hz - cz;
    const float d = zx * yx + zy * yy + zz * yz;
    zx -= d * yx;
    zy -= d * yy;
    zz -= d * yz;
    const float zn = sqrtf(zx * zx + zy * zy + zz * zz);
    if (zn < 1e-4f) {
        HideRay(hand);
        return;
    }
    zx /= zn; zy /= zn; zz /= zn;
    const float xx = yy * zz - yz * zy;
    const float xy = yz * zx - yx * zz;
    const float xz = yx * zy - yy * zx;
    D3DMATRIX m = vrmod::Identity();
    m._11 = xx; m._12 = xy; m._13 = xz;
    m._21 = yx; m._22 = yy; m._23 = yz;
    m._31 = zx; m._32 = zy; m._33 = zz;
    m._41 = cx; m._42 = cy; m._43 = cz;
    vrmod::Get()->SetPanelRay(hand, true, m, length_m, RAY_WIDTH_M);
}

// ---- per-frame -------------------------------------------------------

inline void Close(const char* why) {
    if (open)
        diag::Log("vrkeyboard: closed (%s)", why);
    open = false;
    hover[0] = hover[1] = -1;
    shift = false;
    pending_head = pending_tail = 0;
    key_frames = 0;
    vrmod::Get()->HidePanel();
}


// Detection + open/close edges. Called every controller::OnFrame (before
// the controller decides who owns the frame).
inline void Tick() {
    frame++;
    constexpr bool force_show = false;
    const bool seen = edit_seen_valid && (frame - edit_seen_frame) <= 3u;
    // Log each change of the detector or its cross-checks.
    const int open_bit = BoxOpenBit(), actions = ActionsEnabled(),
              focus = UiFocusWord();
    if (seen != prev_seen || open_bit != prev_open_bit ||
        actions != prev_actions || focus != prev_focus) {
        diag::Log("vrkeyboard: field %s (box 0x%08X open-bit %d, actions-enabled %d, "
                  "ui-focus 0x%02X)",
                  seen ? "OPEN" : "closed", (unsigned)edit_obj, open_bit, actions, focus);
        prev_seen = seen;
        prev_open_bit = open_bit;
        prev_actions = actions;
        prev_focus = focus;
    }
    const bool want = vrmod::config.vr_keyboard &&
                      vrmod::Get()->SupportsHudLayer() &&
                      (seen || force_show);
    if (want && !open) {
        if (!CapturePose())
            return;  // no head pose yet - try again next frame
        open = true;
        shift = false;
        hover[0] = hover[1] = -1;
        prev_trig[0] = prev_trig[1] = true;  // a held trigger must not fire on open
        pending_head = pending_tail = 0;
        key_frames = 0;
        preview[0] = 0;
        dirty = true;
        diag::Log("vrkeyboard: open%s (panel %.2f m ahead, %.2f m down, %.2f m wide)",
                  force_show ? " (forced)" : "", vrmod::config.keyboard_distance_m,
                  vrmod::config.keyboard_drop_m, vrmod::config.keyboard_width_m);
    } else if (!want && open) {
        Close(force_show ? "forced off" : "the field closed");
    }
}

inline void Activate(int key, int hand) {
    const KeyDef& d = keys[key];
    flash_key = key;
    flash_until = frame + 4;
    switch (d.kind) {
    case KK_CHAR: {
        const bool upper = shift != caps;
        QueueChar(upper ? d.shifted : d.ch);
        shift = false;
        break;
    }
    case KK_SPACE:
        QueueChar(' ');
        shift = false;
        break;
    case KK_BACKSPACE:
        PressScancode(SC_BACKSPACE);
        break;
    case KK_ENTER:
        PressScancode(SC_ENTER);
        break;
    case KK_SHIFT:
        shift = !shift;
        break;
    case KK_CAPS:
        caps = !caps;
        break;
    }
    vrmod::Get()->HapticPulse(hand, 0.03f, 0.7f);
    dirty = true;
}

// Drive the open panel with this frame's controller state; adds the
// control keys it is pressing through `add` (the controller's key list).
template <typename AddFn>
inline void Drive(const vrmod::ControllerState& cs, AddFn& add) {
    if (!open)
        return;
    BuildLayout();
    const float press = vrmod::config.controller_press;
    for (int h = 0; h < 2; h++) {
        bool on_panel = false;
        float length_m = RAY_FREE_LENGTH_M;
        AimRay ray;
        const bool have_ray = cs.active[h] && GetAimRay(h, ray);
        const int k = have_ray ? HitKey(ray, on_panel, length_m) : -1;
        if (have_ray)
            ShowRay(h, ray, length_m);
        else
            HideRay(h);
        if (k != hover[h]) {
            hover[h] = k;
            dirty = true;
            if (k >= 0)
                vrmod::Get()->HapticPulse(h, 0.012f, 0.35f);
        }
        const bool trig = cs.active[h] && cs.trigger[h] > press;
        if (trig && !prev_trig[h]) {
            if (k >= 0) {
                Activate(k, h);
            } else if (!on_panel) {
                // Off the panel: right = Enter, left = Backspace.
                PressScancode(h == 1 ? SC_ENTER : SC_BACKSPACE);
                vrmod::Get()->HapticPulse(h, 0.03f, 0.7f);
            }
        }
        prev_trig[h] = trig;
    }
    if (key_frames > 0) {
        key_frames--;
        add(key_sc);
    }
    PostPendingChar();
    if (ReadPreview())
        dirty = true;
    if (flash_key >= 0 && frame == flash_until)
        dirty = true;
    if (dirty) {
        dirty = false;
        Rasterize();
        if (dib_bits != nullptr)
            vrmod::Get()->ShowPanel((const uint32_t*)dib_bits, PANEL_W, PANEL_H,
                                    pose_m, vrmod::config.keyboard_width_m);
    }
}


}  // namespace vrkeyboard
