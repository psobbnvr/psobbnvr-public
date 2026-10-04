#pragma once

// Readied-hotkey highlight ([vr] hotkey_highlight): while a hotbar chord
// holds a technique or attack ready for the swing (controller::HotkeyPress),
// that slot's plate on the game's hotkey bar lights up gold and pulses; the
// swing that fires it flashes the plate white. Items and empty slots press
// at once and never show it.
//
// The art is the bar's own slot plate (texture 0x0C0E27 of the HUD atlas,
// texels 0,0 80x64): its cyan rim and hexagon line become a white mask,
// thickened by a texel, plus a faint fill of the hexagon window the icon
// sits in; the vertex color tints it. Placement follows the bar's window
// (controller::ReadHotbarWindow): plate n sits at the window's position +
// (135 + 70 n, 0) x its scale, the layout of the bar's child table
// 0x9F0BC0 (with the stock 0.6 scale: x 81 + 42 n, y 442, 48x38). Drawn only
// while the bar is open and not sliding.

#include <cmath>
#include <cstdint>
#include <vector>

#include <d3d9.h>
#include <windows.h>

#include "psobbvr_controller.hpp"
#include "psobbvr_log.hpp"
#include "psobbvr_stereo.hpp"
#include "psobbvr_swingind.hpp"
#include "psobbvr_vr.hpp"

namespace hotbarmark {

constexpr uint32_t TEX_ID_PLATE = 0x0C0E27;
constexpr int PLATE_W = 80, PLATE_H = 64;
constexpr int TEX_W = 128, TEX_H = 64;
// A texel inside the plate's hexagon window, where the fill's flood starts.
constexpr int WINDOW_SEED_X = 30, WINDOW_SEED_Y = 26;
constexpr float SLOT_X0 = 135.0f, SLOT_PITCH = 70.0f;  // bar units
// The bar's stock placement, used when the window's own values look wrong.
constexpr float BAR_X_DEFAULT = 0.0f, BAR_Y_DEFAULT = 442.0f;
constexpr float BAR_SCALE_DEFAULT = 0.6f;
constexpr float FILL_ALPHA = 0.22f;
constexpr DWORD COLOR_READY = 0x00FFC030;  // gold; alpha from the pulse
constexpr DWORD COLOR_FIRED = 0x00FFFFFF;
constexpr int PULSE_TICKS = 30;            // one pulse a second
constexpr float PULSE_MIN = 0.55f;
constexpr int FLASH_TICKS = 8;
constexpr BYTE KEY_FIRST = 0x02, KEY_LAST = 0x0B;  // DIK 1..0 = bar slots 0..9

inline IDirect3DTexture9* tex = nullptr;
inline int assets_state = 0;  // 0 untried, 1 loaded, -1 failed (logged)

struct View {
    int slot = -1;  // -1 = nothing to draw
    DWORD color = 0;
    float x = 0.0f, y = 0.0f, scale_x = 0.0f, scale_y = 0.0f;
};
inline View view;
inline int logged_slot = -1;  // the slot whose highlight was last logged

inline void ReleaseAssets() {
    if (tex) { tex->Release(); tex = nullptr; }
    if (assets_state == 1)
        assets_state = 0;  // reload on the next draw
}

inline bool EnsureAssets(IDirect3DDevice9* dev) {
    if (assets_state != 0)
        return assets_state == 1;
    assets_state = -1;
    std::vector<uint8_t> prs, xvm;
    if (!swingind::ReadGslEntry(swingind::ATLAS_ENTRY, prs))
        return false;
    if (!swingind::PrsDecompress(prs, xvm)) {
        diag::Log("hotbarmark: PRS decompression of %s failed", swingind::ATLAS_ENTRY);
        return false;
    }
    const uint8_t* data = nullptr;
    uint32_t fmt = 0, size = 0;
    unsigned w = 0, h = 0;
    std::vector<uint32_t> px;
    if (!swingind::FindXvr(xvm, TEX_ID_PLATE, data, fmt, w, h, size) ||
        fmt != 7 || w < (unsigned)PLATE_W || h < (unsigned)PLATE_H ||
        !swingind::DecodeDxt3(data, size, w, h, px)) {
        diag::Log("hotbarmark: texture 0x%06X missing or unexpected in %s",
                  TEX_ID_PLATE, swingind::ATLAS_ENTRY);
        return false;
    }
    // Outline weight from the texel's brightest channel: the rim and the
    // hexagon line are bright, the plate's body and tab dark.
    std::vector<float> line((size_t)PLATE_W * PLATE_H, 0.0f);
    for (int y = 0; y < PLATE_H; y++) {
        for (int x = 0; x < PLATE_W; x++) {
            const uint32_t p = px[(size_t)y * w + x];
            const int a = (int)(p >> 24), r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
            const int m = r > g ? (r > b ? r : b) : (g > b ? g : b);
            float t = (m - 60) / 140.0f;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            line[(size_t)y * PLATE_W + x] = t * a / 255.0f;
        }
    }
    // The hexagon window: the transparent texels reachable from the seed.
    std::vector<uint8_t> inside((size_t)PLATE_W * PLATE_H, 0);
    std::vector<int> stack;
    stack.push_back(WINDOW_SEED_Y * PLATE_W + WINDOW_SEED_X);
    while (!stack.empty()) {
        const int i = stack.back();
        stack.pop_back();
        const int x = i % PLATE_W, y = i / PLATE_W;
        if (inside[i] || (px[(size_t)y * w + x] >> 24) >= 32)
            continue;
        inside[i] = 1;
        if (x > 0) stack.push_back(i - 1);
        if (x < PLATE_W - 1) stack.push_back(i + 1);
        if (y > 0) stack.push_back(i - PLATE_W);
        if (y < PLATE_H - 1) stack.push_back(i + PLATE_W);
    }
    std::vector<uint32_t> out((size_t)TEX_W * TEX_H, 0);
    for (int y = 0; y < PLATE_H; y++) {
        for (int x = 0; x < PLATE_W; x++) {
            float a = 0.0f;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && nx < PLATE_W && ny >= 0 && ny < PLATE_H)
                        a = fmaxf(a, line[(size_t)ny * PLATE_W + nx]);
                }
            if (inside[(size_t)y * PLATE_W + x])
                a = fmaxf(a, FILL_ALPHA);
            out[(size_t)y * TEX_W + x] = ((uint32_t)(a * 255.0f + 0.5f) << 24) | 0x00FFFFFFu;
        }
    }
    tex = swingind::MakeTexture(dev, TEX_W, TEX_H, out.data());
    if (tex == nullptr) {
        diag::Log("hotbarmark: texture creation failed");
        return false;
    }
    assets_state = 1;
    diag::Log("hotbarmark: assets loaded from %s (plate 0x%06X)",
              swingind::ATLAS_ENTRY, TEX_ID_PLATE);
    return true;
}

// Once per frame after controller::OnFrame: which slot to light, how.
inline void Update(bool gameplay_drives) {
    view = View{};
    if (!vrmod::config.hotkey_highlight || !gameplay_drives) {
        logged_slot = -1;
        return;
    }
    const unsigned long now = controller::GameTicks();
    const BYTE armed = controller::hk_armed_key;
    const BYTE fired = controller::hk_fired_key;
    int slot = -1;
    float alpha = 0.0f;
    DWORD rgb = 0;
    if (armed >= KEY_FIRST && armed <= KEY_LAST) {
        slot = armed - KEY_FIRST;
        const float phase = (float)(now % PULSE_TICKS) / PULSE_TICKS;
        alpha = PULSE_MIN + (1.0f - PULSE_MIN) *
                (0.5f + 0.5f * cosf(phase * 6.2831853f));
        rgb = COLOR_READY;
    } else if (fired >= KEY_FIRST && fired <= KEY_LAST &&
               now - controller::hk_fired_tick < (unsigned long)FLASH_TICKS) {
        slot = fired - KEY_FIRST;
        alpha = 1.0f - (float)(now - controller::hk_fired_tick) / FLASH_TICKS;
        rgb = COLOR_FIRED;
    }
    if (slot < 0) {
        logged_slot = -1;
        return;
    }
    controller::HotbarWindow bar;
    if (!controller::ReadHotbarWindow(bar) || !bar.w.visible ||
        (bar.w.state & controller::WINDOW_STATE_OPEN) == 0 ||
        (bar.w.state & controller::WINDOW_STATE_SLIDING) != 0)
        return;
    float x = bar.x, y = bar.y, sx = bar.scale_x, sy = bar.scale_y;
    bool fallback = false;
    if (!(x >= -100.0f && x <= 640.0f && y >= 0.0f && y <= 480.0f)) {
        x = BAR_X_DEFAULT;
        y = BAR_Y_DEFAULT;
        fallback = true;
    }
    if (!(sx > 0.1f && sx < 4.0f && sy > 0.1f && sy < 4.0f)) {
        sx = sy = BAR_SCALE_DEFAULT;
        fallback = true;
    }
    if (slot != logged_slot) {
        logged_slot = slot;
        diag::Log("hotbarmark: hotkey %d lit (%s) - bar at (%.1f, %.1f) scale "
                  "%.2f x %.2f%s", (slot + 1) % 10,
                  rgb == COLOR_READY ? "ready" : "fired", x, y, sx, sy,
                  fallback ? " (stock placement: the window's values looked wrong)" : "");
    }
    const uint32_t a8 = (uint32_t)(alpha * 255.0f + 0.5f);
    view.slot = slot;
    view.color = (a8 << 24) | rgb;
    view.x = x;
    view.y = y;
    view.scale_x = sx;
    view.scale_y = sy;
}

// Called from Present beside swingind::Draw, on top of the game's HUD.
inline void Draw(IDirect3DDevice9* dev) {
    if (view.slot < 0)
        return;
    if (!stereo::HudLayerActive() || stereo::hud_rt == nullptr ||
        stereo::hud_width == 0)
        return;
    if (!EnsureAssets(dev))
        return;
    swingind::HudSpriteScope scope(dev);
    const float hw = PLATE_W * view.scale_x * 0.5f, hh = PLATE_H * view.scale_y * 0.5f;
    const float cx = view.x + (SLOT_X0 + SLOT_PITCH * view.slot) * view.scale_x + hw;
    const float cy = view.y + hh;
    dev->SetTexture(0, tex);
    swingind::Quad(dev, cx, cy, hw, hh, 0.0f, 0.0f, (float)PLATE_W / TEX_W,
                   (float)PLATE_H / TEX_H, view.color);
}

}  // namespace hotbarmark
