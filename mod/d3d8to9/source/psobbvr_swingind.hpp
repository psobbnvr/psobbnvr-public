#pragma once

// Swing-timing indicator ([vr] swing_indicator): a HUD sprite showing what
// the next physical swing does, since the character's animation is not
// visible in first person.
//
//   hexagon  green = a swing starts an attack (fresh, the combo's next
//                    step, or a held charge)
//            blue  = a swing lands the next strike of the current attack
//            grey  = not yet (swing in flight, clip running, charge
//                    counting down, chain taken, follow-through, blend to
//                    idle, cast)
//            red   = flashes after a press that poisoned the step
//   ring     the time until the hexagon lights: shrinks from full size,
//            hovers just outside the hexagon, and lands on it for a few
//            ticks on the tick it lights. After the combo's last (or a
//            poisoned) step it runs through the follow-through and the
//            blend back to idle.
// Strikes within a multi-hit step show only a short grey blink. It shows
// only while the game's command palette does (controller::PaletteVisible).
//
// Textures come from the game's HUD atlas f256_hyouji.prs in
// data\data.gsl (a PRS-compressed XVM), read at first use: 0x0C0242 has
// the small hexagons in its bottom row (red, green, orange, blue; grey is
// made from the green), 0x0C0238 is a 4x4 sheet of 64x64 spinning ring
// frames on black, drawn additively by the game. Additive light is lost
// over empty parts of the HUD quad (composited by alpha), so the ring
// gets alpha = brightness and is alpha-blended instead.
//
// Formats: data.gsl = 48-byte index entries (name[32], u32 offset in
// 2048-byte sectors, u32 size) up to an empty name. PRS = Sega's LZ77
// (control bits LSB-first: 1 = literal; 0 1 = long copy, u16 a: offset =
// (a >> 3) | ~0x1FFF, a == 0 ends, count = (a & 7) + 2, or next byte + 1
// when those bits are 0; 0 0 = short copy, two bits + 2, offset = u8 |
// ~0xFF). XVM = "XVMH" (u32 size, u32 count) then "XVRT" chunks: u32
// size, u32 flags, u32 format (6 = DXT1, 7 = DXT3), u32 texture id, u16
// width, u16 height, u32 data size, 0x20 bytes pad, pixels
// (tools\xvmdump.py is the same parser in Python).

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <d3d9.h>
#include <windows.h>

#include "psobbvr_controller.hpp"
#include "psobbvr_gamecam.hpp"
#include "psobbvr_log.hpp"
#include "psobbvr_stereo.hpp"
#include "psobbvr_vr.hpp"
#include "psobbvr_weapongrip.hpp"

namespace swingind {

constexpr uint32_t TEX_ID_HEX = 0x0C0242;
constexpr uint32_t TEX_ID_RING = 0x0C0238;
constexpr const char* ATLAS_ENTRY = "f256_hyouji.prs";

// Cells of the hexagon atlas we build, 34x26 texels side by side (the
// source's pitch; wider picks up a neighbouring plate's rim).
enum { HEX_RED = 0, HEX_GREEN, HEX_BLUE, HEX_GREY, HEX_COUNT, HEX_NONE = -1 };
constexpr int HEX_CELL_W = 34, HEX_CELL_H = 26;
constexpr int HEX_ATLAS_W = 256, HEX_ATLAS_H = 32;
// Source cells in texture 0x0C0242: hexagon center x (each 25x21 texels),
// rows 203..228.
constexpr float HEX_SRC_CX[3] = {16.5f, 50.5f, 118.5f};
constexpr int HEX_SRC_Y0 = 203;
constexpr float HEX_GREY_LEVEL = 0.6f;  // grey = the green's luminance x this
constexpr int RING_FRAMES = 16;

struct View {
    int hex = HEX_NONE;
    bool ring = false;
    float ring_t = 1.0f;  // 1 = full radius, 0 = touching the hexagon
    bool land = false;    // drawn at its end size on the lit hexagon
};
inline View view;

// ---- assets ---------------------------------------------------------------

inline IDirect3DTexture9* tex_hex = nullptr;
inline IDirect3DTexture9* tex_ring = nullptr;
inline int assets_state = 0;  // 0 untried, 1 loaded, -1 failed (logged)

inline bool PrsDecompress(const std::vector<uint8_t>& in,
                          std::vector<uint8_t>& out) {
    size_t pos = 0;
    const size_t n = in.size();
    unsigned ctrl = 0;
    int bits = 0;
    auto bit = [&]() -> int {
        if (bits == 0) {
            if (pos >= n)
                return -1;
            ctrl = in[pos++];
            bits = 8;
        }
        const int b = ctrl & 1;
        ctrl >>= 1;
        bits--;
        return b;
    };
    out.clear();
    out.reserve(in.size() * 4);
    while (pos < n) {
        int b = bit();
        if (b < 0)
            break;
        if (b) {
            if (pos >= n)
                return false;
            out.push_back(in[pos++]);
            continue;
        }
        b = bit();
        if (b < 0)
            break;
        long offset;
        size_t count;
        if (b) {
            if (pos + 1 >= n)
                return false;
            const unsigned a = in[pos] | (in[pos + 1] << 8);
            pos += 2;
            offset = (long)(a >> 3) | ~0x1FFFL;
            if (offset == ~0x1FFFL)
                break;  // end marker
            if (a & 7) {
                count = (a & 7) + 2;
            } else {
                if (pos >= n)
                    return false;
                count = in[pos++] + 1;
            }
        } else {
            const int b1 = bit(), b0 = bit();
            if (b1 < 0 || b0 < 0)
                return false;
            count = (size_t)((b1 << 1) | b0) + 2;
            if (pos >= n)
                return false;
            offset = (long)in[pos++] | ~0xFFL;
        }
        const long src = (long)out.size() + offset;
        if (src < 0)
            return false;
        for (size_t i = 0; i < count; i++)
            out.push_back(out[(size_t)src + i]);
    }
    return true;
}

// Read one entry of data.gsl next to the game executable.
inline bool ReadGslEntry(const char* name, std::vector<uint8_t>& out) {
    char path[MAX_PATH];
    const DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return false;
    char* slash = strrchr(path, '\\');
    if (slash == nullptr)
        return false;
    *(slash + 1) = 0;
    strcat_s(path, "data\\data.gsl");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || f == nullptr) {
        diag::Log("swingind: cannot open %s", path);
        return false;
    }
    bool ok = false;
    const size_t index_bytes = 48 * 2048;
    std::vector<uint8_t> index(index_bytes);
    const size_t got = fread(index.data(), 1, index_bytes, f);
    for (size_t off = 0; off + 48 <= got; off += 48) {
        if (index[off] == 0)
            break;
        char ename[33];
        memcpy(ename, &index[off], 32);
        ename[32] = 0;
        if (_stricmp(ename, name) != 0)
            continue;
        uint32_t sector = 0, size = 0;
        memcpy(&sector, &index[off + 32], 4);
        memcpy(&size, &index[off + 36], 4);
        if (size == 0 || size > (64u << 20))
            break;
        out.resize(size);
        if (_fseeki64(f, (long long)sector * 2048, SEEK_SET) == 0 &&
            fread(out.data(), 1, size, f) == size)
            ok = true;
        break;
    }
    fclose(f);
    if (!ok)
        diag::Log("swingind: entry %s not found in data.gsl", name);
    return ok;
}

// Locate one XVRT chunk by texture id: pixels + format + size.
inline bool FindXvr(const std::vector<uint8_t>& xvm, uint32_t id,
                    const uint8_t*& data, uint32_t& fmt, unsigned& w,
                    unsigned& h, uint32_t& dsize) {
    if (xvm.size() < 8 || memcmp(xvm.data(), "XVMH", 4) != 0)
        return false;
    size_t off = 8;
    uint32_t chunk = 0;
    memcpy(&chunk, &xvm[4], 4);
    off += chunk;
    while (off + 0x40 <= xvm.size()) {
        if (memcmp(&xvm[off], "XVRT", 4) != 0)
            return false;
        uint32_t size = 0, f = 0, tid = 0, ds = 0;
        uint16_t tw = 0, th = 0;
        memcpy(&size, &xvm[off + 4], 4);
        memcpy(&f, &xvm[off + 12], 4);
        memcpy(&tid, &xvm[off + 16], 4);
        memcpy(&tw, &xvm[off + 20], 2);
        memcpy(&th, &xvm[off + 22], 2);
        memcpy(&ds, &xvm[off + 24], 4);
        if (tid == id) {
            if (off + 0x40 + ds > xvm.size())
                return false;
            data = &xvm[off + 0x40];
            fmt = f;
            w = tw;
            h = th;
            dsize = ds;
            return true;
        }
        off += 8 + size;
    }
    return false;
}

inline uint32_t Rgb565(uint16_t c) {
    const unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
}

// DXT3 -> 0xAARRGGBB, row-major.
inline bool DecodeDxt3(const uint8_t* src, uint32_t size, unsigned w,
                       unsigned h, std::vector<uint32_t>& out) {
    if (w % 4 || h % 4 || size < (w / 4) * (h / 4) * 16)
        return false;
    out.assign((size_t)w * h, 0);
    const uint8_t* p = src;
    for (unsigned by = 0; by < h; by += 4) {
        for (unsigned bx = 0; bx < w; bx += 4, p += 16) {
            uint16_t c0, c1;
            memcpy(&c0, p + 8, 2);
            memcpy(&c1, p + 10, 2);
            uint32_t pal[4];
            pal[0] = Rgb565(c0);
            pal[1] = Rgb565(c1);
            for (int k = 0; k < 2; k++) {
                const uint32_t a = pal[0], b = pal[1];
                uint32_t v = 0;
                for (int sh = 0; sh <= 16; sh += 8) {
                    const unsigned ca = (a >> sh) & 255, cb = (b >> sh) & 255;
                    const unsigned m = k == 0 ? (2 * ca + cb) / 3 : (ca + 2 * cb) / 3;
                    v |= m << sh;
                }
                pal[2 + k] = v;
            }
            for (unsigned y = 0; y < 4; y++) {
                const unsigned arow = p[y * 2] | (p[y * 2 + 1] << 8);
                const unsigned idx = p[12 + y];
                for (unsigned x = 0; x < 4; x++) {
                    const unsigned a4 = (arow >> (x * 4)) & 15;
                    const unsigned ci = (idx >> (x * 2)) & 3;
                    out[(size_t)(by + y) * w + bx + x] =
                        ((a4 * 17) << 24) | pal[ci];
                }
            }
        }
    }
    return true;
}

// Grey: the texel's luminance scaled by HEX_GREY_LEVEL, alpha kept.
inline uint32_t Grey(uint32_t argb) {
    const float r = (float)((argb >> 16) & 255), g = (float)((argb >> 8) & 255),
                b = (float)(argb & 255);
    const float l = (0.299f * r + 0.587f * g + 0.114f * b) * HEX_GREY_LEVEL;
    const uint32_t v = (uint32_t)(fminf(fmaxf(l, 0.0f), 255.0f) + 0.5f);
    return (argb & 0xFF000000u) | (v << 16) | (v << 8) | v;
}

inline IDirect3DTexture9* MakeTexture(IDirect3DDevice9* dev, unsigned w,
                                      unsigned h, const uint32_t* px) {
    IDirect3DTexture9* t = nullptr;
    if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8,
                                  D3DPOOL_DEFAULT, &t, nullptr)) ||
        t == nullptr)
        return nullptr;
    D3DLOCKED_RECT lr;
    if (FAILED(t->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) {
        t->Release();
        return nullptr;
    }
    for (unsigned y = 0; y < h; y++)
        memcpy((uint8_t*)lr.pBits + (size_t)y * lr.Pitch, px + (size_t)y * w,
               (size_t)w * 4);
    t->UnlockRect(0);
    return t;
}

inline void ReleaseAssets() {
    if (tex_hex) { tex_hex->Release(); tex_hex = nullptr; }
    if (tex_ring) { tex_ring->Release(); tex_ring = nullptr; }
    if (assets_state == 1)
        assets_state = 0;  // reload on the next draw
}

inline bool EnsureAssets(IDirect3DDevice9* dev) {
    if (assets_state != 0)
        return assets_state == 1;
    assets_state = -1;
    std::vector<uint8_t> prs, xvm;
    if (!ReadGslEntry(ATLAS_ENTRY, prs))
        return false;
    if (!PrsDecompress(prs, xvm)) {
        diag::Log("swingind: PRS decompression of %s failed", ATLAS_ENTRY);
        return false;
    }
    const uint8_t* hex_data = nullptr;
    const uint8_t* ring_data = nullptr;
    uint32_t hf = 0, rf = 0, hs = 0, rs = 0;
    unsigned hw = 0, hh = 0, rw = 0, rh = 0;
    if (!FindXvr(xvm, TEX_ID_HEX, hex_data, hf, hw, hh, hs) ||
        !FindXvr(xvm, TEX_ID_RING, ring_data, rf, rw, rh, rs)) {
        diag::Log("swingind: texture 0x%06X / 0x%06X not found in %s",
                  TEX_ID_HEX, TEX_ID_RING, ATLAS_ENTRY);
        return false;
    }
    std::vector<uint32_t> hex_px, ring_px;
    if (hf != 7 || rf != 7 || !DecodeDxt3(hex_data, hs, hw, hh, hex_px) ||
        !DecodeDxt3(ring_data, rs, rw, rh, ring_px) || rw != 256 || rh != 256) {
        diag::Log("swingind: unexpected texture format (%u %ux%u / %u %ux%u)",
                  hf, hw, hh, rf, rw, rh);
        return false;
    }
    // The hexagon atlas: three game cells plus the grey one from the green.
    std::vector<uint32_t> atlas((size_t)HEX_ATLAS_W * HEX_ATLAS_H, 0);
    for (int cell = 0; cell < HEX_COUNT; cell++) {
        const int src_cell = cell == HEX_GREY ? HEX_GREEN : cell;
        const int sx0 = (int)(HEX_SRC_CX[src_cell] - HEX_CELL_W / 2 + 0.5f);
        for (int y = 0; y < HEX_CELL_H; y++) {
            const int sy = HEX_SRC_Y0 + y;
            if (sy < 0 || sy >= (int)hh)
                continue;
            for (int x = 0; x < HEX_CELL_W; x++) {
                const int sx = sx0 + x;
                if (sx < 0 || sx >= (int)hw)
                    continue;
                uint32_t p = hex_px[(size_t)sy * hw + sx];
                if (cell == HEX_GREY)
                    p = Grey(p);
                atlas[(size_t)y * HEX_ATLAS_W + cell * HEX_CELL_W + x] = p;
            }
        }
    }
    // The ring: alpha = brightness.
    std::vector<uint32_t> ring(ring_px.size());
    for (size_t i = 0; i < ring_px.size(); i++) {
        const uint32_t p = ring_px[i] & 0x00FFFFFFu;
        const uint32_t r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
        const uint32_t a = r > g ? (r > b ? r : b) : (g > b ? g : b);
        ring[i] = (a << 24) | p;
    }
    tex_hex = MakeTexture(dev, HEX_ATLAS_W, HEX_ATLAS_H, atlas.data());
    tex_ring = MakeTexture(dev, 256, 256, ring.data());
    if (tex_hex == nullptr || tex_ring == nullptr) {
        diag::Log("swingind: texture creation failed");
        ReleaseAssets();
        assets_state = -1;
        return false;
    }
    assets_state = 1;
    diag::Log("swingind: assets loaded from %s (hex 0x%06X %ux%u, ring "
              "0x%06X %ux%u)", ATLAS_ENTRY, TEX_ID_HEX, hw, hh, TEX_ID_RING,
              rw, rh);
    return true;
}

// ---- the state machine ----------------------------------------------------

constexpr int BLINK_TICKS = 4;        // grey after a strike lands, min
constexpr int FLASH_TICKS = 24;       // red flash: 3 x (4 on, 4 off)
constexpr int FIRE_LATENCY_TICKS = 15; // a fired swing whose attack never came
constexpr int LAND_TICKS = 3;          // the landed ring stays this long
constexpr float RING_HOVER_PX = 8.0f;  // the approach stops this far (640x480
                                       // px) outside the end size; only the
                                       // landing touches
// Follow-through countdown the last-hit setter 0x6A2FB4 writes to +0x8BC:
// byte table 0x969078[kind * 3 + step]. It runs after the clip ends; the
// combo-window stretch is added only when a next step exists, so a last
// step's value is the table's.
constexpr uintptr_t FOLLOW_THROUGH_TABLE = 0x969078;
// Stage 4's blend back to idle, in ticks, until the first one is measured.
constexpr float IDLE_BLEND_TICKS_INIT = 3.0f;
constexpr int MODE_IDLE = 1;
// Action modes 5 / 6 / 7 are the combo's three steps; palette tech types
// 3 / 5 start casts in modes 8 / 9.
constexpr int MODE_CAST_A = 8, MODE_CAST_B = 9;
// A charge's countdown (+0x8BC) starts at 6; stage 2 counts it down and
// the charge hold pins it at controller::CHARGE_HOLD_TICKS.
constexpr int CHARGE_START_TICKS = 6;

inline LONGLONG seen_fire_qpc = 0;
inline unsigned long fire_tick = 0;
inline bool fire_pending = false;
inline bool cast_prev = false;   // last tick was a technique cast (modes 8 / 9)
// A cast runs 33 ticks: 5 blending in, 10 to the hold frame, 11 held
// with the clock frozen, 5 blending to idle. The ring spans all of it
// (the clip alone would stall it through the hold); re-measured at each
// cast's end.
constexpr float CAST_TICKS_INIT = 33.0f;
inline float cast_total_ticks = CAST_TICKS_INIT;
inline unsigned long cast_start_tick = 0;
inline bool was_stage3 = false;
inline int step_prev = -1;
inline short hit_ctr_prev = 0;
inline bool hit_landed = false;
inline unsigned long hit_tick = 0;
inline float ring_total = -1.0f;
inline bool ring_prev = false;
inline uint32_t lc_prev = 0;
inline unsigned long flash_until = 0;
inline unsigned long land_until = 0;
inline float idle_blend_ticks = IDLE_BLEND_TICKS_INIT;  // measured at each exit
inline bool stage4_seen = false;
inline unsigned long stage4_tick = 0;
inline short anim_prev = 0;     // last frame's clip id, length, step and
inline float len_prev = 0.0f;   // clock: a change (or a backward clock)
inline float astep_prev = 0.0f; // under the ring = the clip restarted,
inline float clock_prev = -1.0f; // rescale it
inline unsigned long long modes_logged = 0;
inline View view_prev;
inline bool palette_prev = true;  // the palette gate's last value (for the log line)

inline void ResetState() {
    fire_pending = false;
    cast_prev = false;
    was_stage3 = false;
    step_prev = -1;
    hit_landed = false;
    ring_total = -1.0f;
    ring_prev = false;
    lc_prev = 0;
    land_until = 0;
    stage4_seen = false;
}


inline void Update(bool gameplay_drives) {
    auto& c = vrmod::config;
    View v;
    const bool palette_ok = controller::PaletteVisible();
    if (c.swing_indicator && palette_ok != palette_prev)
        diag::Log("swingind: %s (the command palette %s)",
                  palette_ok ? "shown" : "hidden",
                  palette_ok ? "is showing" : "is not showing");
    palette_prev = palette_ok;
    const bool on = c.swing_indicator && gameplay_drives && palette_ok &&
                    c.swing_attack != 0 && weapongrip::SwingWeapon();
    uintptr_t entity = on ? gamecam::ResolveEntity() : 0;
    if (entity == 0 ||
        !diag::Accessible(entity + controller::ENT_ACTION_MODE_OFF, 4, false) ||
        !diag::Accessible(entity + controller::ENT_ANIM_CUR_OFF, 0x14, false) ||
        !diag::Accessible(entity + controller::ENT_ATK_VARIANT_OFF, 0x20, false) ||
        !diag::Accessible(entity + 0xB0, 4, false)) {
        ResetState();
        view = v;
        return;
    }
    const unsigned long now = controller::GameTicks();
    const int mode = *reinterpret_cast<const short*>(entity + controller::ENT_ACTION_MODE_OFF);
    const int stage = *reinterpret_cast<const short*>(entity + controller::ENT_ACTION_STAGE_OFF);
    const float clock = *reinterpret_cast<const float*>(entity + controller::ENT_ANIM_CLOCK_OFF);
    const float step = *reinterpret_cast<const float*>(entity + controller::ENT_ANIM_STEP_OFF);
    const float len = *reinterpret_cast<const float*>(entity + 0xC4);
    const uint32_t flags = *reinterpret_cast<const uint32_t*>(entity + 0xB0);
    const uint32_t lc = *reinterpret_cast<const uint32_t*>(entity + 0x8AC);
    const int combo_step = *reinterpret_cast<const int*>(entity + 0x8B4);
    const int cd = *reinterpret_cast<const int*>(entity + controller::ENT_STAGE_TIMER_OFF);
    const short hit_ctr = *reinterpret_cast<const short*>(entity + 0x8B2);
    const short anim_id = *reinterpret_cast<const short*>(entity + controller::ENT_ANIM_CUR_OFF);
    const bool attack = mode >= 5 && mode <= 7;
    const bool cast = mode == MODE_CAST_A || mode == MODE_CAST_B;
    if (cast && !cast_prev)
        cast_start_tick = now;
    if (!cast && cast_prev) {
        const float measured = fminf(90.0f, fmaxf(10.0f, (float)(now - cast_start_tick)));
        if (fabsf(measured - cast_total_ticks) >= 1.0f)
            diag::Log("swingind: the cast took %.0f ticks (the ring was pacing on %.0f)",
                      measured, cast_total_ticks);
        cast_total_ticks = measured;
    }

    // A swing fired: the press is in flight until the attack shows up.
    if (controller::sw_fire_qpc != seen_fire_qpc) {
        seen_fire_qpc = controller::sw_fire_qpc;
        if (!attack) {
            fire_pending = true;
            fire_tick = now;
        }
    }
    // A cast also ends the in-flight state (with cast_swing the swing
    // casts), so the ring does not hang at full size over it.
    if (attack || cast || (fire_pending && now - fire_tick > (unsigned long)FIRE_LATENCY_TICKS))
        fire_pending = false;
    // A poisoned step: the anti-mash bit rising during an attack.
    if (attack && (lc & 0x04) && !(lc_prev & 0x04)) {
        flash_until = now + FLASH_TICKS;
        diag::Log("swingind: step %d poisoned (press before the window "
                  "opened) - red flash", combo_step);
    }
    lc_prev = attack ? lc : 0;
    // Log each other action mode once (the idle rule below treats them as
    // ready).
    if (mode >= 0 && mode < 64 && !(modes_logged & (1ull << mode)) &&
        mode != MODE_IDLE && !attack) {
        modes_logged |= 1ull << mode;
        diag::Log("swingind: action mode %d seen (shown as %s)", mode,
                  mode == MODE_CAST_A || mode == MODE_CAST_B ? "busy" : "ready");
    }

    bool ring_now = false;
    float rem = -1.0f;        // the ring's ticks to go; < 0 = no ring here
    bool ring_reset = false;  // a new clip under the ring: rescale it
    bool ring_unscaled = false;  // no scale yet: full size until there is
    if (attack && stage == 3) {
        stage4_seen = false;
        if (!was_stage3 || combo_step != step_prev) {
            step_prev = combo_step;
            hit_ctr_prev = hit_ctr;
            hit_landed = false;
            ring_total = -1.0f;
        }
        was_stage3 = true;
        if (hit_ctr != hit_ctr_prev) {
            hit_ctr_prev = hit_ctr;
            hit_landed = true;
            hit_tick = now;
        }
        const bool strike_waiting = controller::SwingHoldWaiting();
        // Ticks the clock will still spend paused this step: the post-hit
        // pause running, or the one the warp's skip will cost once the
        // hit lands.
        float pause_ahead = 0.0f;
        if (controller::wp_hold == 1) {
            const int left = controller::wp_hold_left - controller::wp_hold_ticks;
            pause_ahead = left > 0 ? (float)left : 0.0f;
        } else if (controller::wp_ours_paced && !hit_landed &&
                   controller::wp_skipped_ticks > 0) {
            pause_ahead = (float)(controller::wp_skipped_ticks + 1);
        }
        // The clip end is the game's flag (+0xB0 bit 0x10), not the clock:
        // the advance 0x7AA094 ends a one-shot clip on the tick its step
        // would reach (length - 1) and undoes that step, so a clock past
        // the end is only our post-hit pause value. Ticks to the flag =
        // advances still needed to reach (length - 1), at least one.
        const bool clip_ended = (flags & 0x10) != 0;
        const float clip_left = step > 0.0f ? fmaxf(0.0f, (len - 1.0f - clock) / step) + 1.0f : 1.0f;
        const bool window_open = (lc & 0x02) != 0 && (lc & 0x24) == 0;
        // Bit 0 = action end: a chain accepted, or the window expired.
        const bool action_end = (lc & 0x01) != 0;
        // A step that cannot chain (the third, or a poisoned one) ends
        // through the follow-through countdown (the table value until the
        // clip ends, then +0x8BC live) and stage 4's blend back to idle;
        // its ring runs on to that idle green.
        const bool ends_idle = combo_step >= 2 || (lc & 0x04) != 0;
        float tail = 0.0f;
        if (ends_idle) {
            float follow = clip_ended ? (float)(cd > 0 ? cd : 0) : 0.0f;
            if (!clip_ended) {
                const int kind = controller::MeleeKind(entity);
                const uintptr_t at = FOLLOW_THROUGH_TABLE +
                                     (uintptr_t)(kind * 3 + combo_step);
                if (kind >= 0 && combo_step >= 0 && combo_step < 3 &&
                    diag::Accessible(at, 1, false))
                    follow = (float)*reinterpret_cast<const uint8_t*>(at);
            }
            tail = follow + idle_blend_ticks + 1.0f;
        }
        if (strike_waiting) {
            v.hex = (hit_landed && now - hit_tick < (unsigned long)BLINK_TICKS) ? HEX_GREY : HEX_BLUE;
        } else if (clip_ended && !ends_idle) {
            // Window open, no press taken, a step left: a swing chains the
            // next step (green). Taken or expired: grey until the next
            // step starts or the action ends.
            v.hex = (window_open && !action_end && combo_step < 2) ? HEX_GREEN : HEX_GREY;
        } else if (clip_ended) {
            // The last step's follow-through: the ring on its way to the
            // idle green.
            v.hex = HEX_GREY;
            rem = tail;
        } else {
            v.hex = HEX_GREY;
            rem = pause_ahead + clip_left + tail;
            // A fresh attack's first stage-3 tick plays the clip at its
            // unscaled step, then restarts it, so the ring's scale waits
            // for the warp's decision (controller::wp_done), holding full
            // size until then, and is retaken on any restart sign: clip id,
            // length or step changing, or the clock jumping backward.
            if (anim_id != anim_prev || len != len_prev || step != astep_prev ||
                clock < clock_prev - 0.001f)
                ring_reset = true;
            if (!controller::wp_done)
                ring_unscaled = true;
        }
    } else if (attack && (stage == 1 || stage == 2)) {
        // A heavy/special's charge: grey, the ring shrinking on the game's
        // countdown (stage 1 waits at its start, stage 2 counts down).
        // Once the charge hold pins it, a swing releases at once: green.
        // A native charge is never pinned and stays grey.
        was_stage3 = false;
        const bool ready = controller::ch_holding &&
                           cd <= controller::CHARGE_HOLD_TICKS;
        if (ready) {
            v.hex = HEX_GREEN;
        } else {
            v.hex = HEX_GREY;
            v.ring = true;
            v.ring_t = fminf(1.0f, fmaxf(0.0f,
                (float)(cd - controller::CHARGE_HOLD_TICKS) /
                (float)(CHARGE_START_TICKS - controller::CHARGE_HOLD_TICKS)));
            ring_now = true;
        }
    } else if (attack) {
        // Stage 4 (the blend back to idle) or unknown: grey. A running
        // ring continues through the blend, whose length is measured at
        // the exit for next time.
        was_stage3 = false;
        v.hex = HEX_GREY;
        if (!stage4_seen) {
            stage4_seen = true;
            stage4_tick = now;
        }
        if (ring_prev)
            rem = fmaxf(0.0f, idle_blend_ticks + 1.0f - (float)(now - stage4_tick));
    } else {
        was_stage3 = false;
        step_prev = -1;
        if (stage4_seen) {
            stage4_seen = false;
            const float measured = fminf(15.0f, fmaxf(1.0f, (float)(now - stage4_tick)));
            if (fabsf(measured - idle_blend_ticks) >= 1.0f)
                diag::Log("swingind: the blend back to idle took %.0f ticks "
                          "(was pacing on %.0f)", measured, idle_blend_ticks);
            idle_blend_ticks = measured;
        }
        if (fire_pending) {
            v.hex = HEX_GREY;
            v.ring = true;
            v.ring_t = 1.0f;
            ring_now = true;
            ring_total = -1.0f;
        } else if (cast) {
            // A cast: grey, the ring spanning cast_total_ticks, scaled
            // once on entry (the clip phases inside are not restarts).
            v.hex = HEX_GREY;
            rem = fmaxf(0.0f, cast_total_ticks - (float)(now - cast_start_tick));
            if (!cast_prev)
                ring_reset = true;
        } else {
            v.hex = HEX_GREEN;
        }
    }
    if (rem >= 0.0f) {
        ring_now = true;
        if (!ring_prev || ring_reset)
            ring_total = -1.0f;
        if (ring_total < 0.0f && rem > 0.0f && !ring_unscaled)
            ring_total = rem;
        v.ring = true;
        v.ring_t = ring_total > 0.0f ? fminf(1.0f, fmaxf(0.0f, rem / ring_total)) : 1.0f;
    }
    // The landing: a nearly closed ring that ends as the hexagon lights is
    // drawn at its end size for LAND_TICKS. One still far out just goes;
    // the hexagon going unlit drops it.
    const bool lit = v.hex == HEX_GREEN || v.hex == HEX_BLUE;
    if (ring_prev && !ring_now && lit && view_prev.ring_t <= 0.5f)
        land_until = now + LAND_TICKS;
    if (!ring_now && lit && now < land_until) {
        v.ring = true;
        v.ring_t = 0.0f;
        v.land = true;
    } else if (!ring_now) {
        land_until = 0;
    }
    ring_prev = ring_now;
    if (now < flash_until) {
        const unsigned long left = flash_until - now;
        v.hex = ((left / 4) & 1) ? HEX_RED : HEX_NONE;
    }
    cast_prev = cast;
    anim_prev = anim_id;
    len_prev = len;
    astep_prev = step;
    clock_prev = clock;
    view_prev = v;
    view = v;
}

// ---- drawing --------------------------------------------------------------

struct Vtx {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};
constexpr DWORD VTX_FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

inline void Quad(IDirect3DDevice9* dev, float cx, float cy, float hw, float hh,
                 float u0, float v0, float u1, float v1, DWORD color) {
    const float sx = stereo::hud_width / resolution::kGameWidth;
    const float sy = stereo::hud_height / resolution::kGameHeight;
    const float x0 = (cx - hw) * sx - 0.5f, x1 = (cx + hw) * sx - 0.5f;
    const float y0 = (cy - hh) * sy - 0.5f, y1 = (cy + hh) * sy - 0.5f;
    const Vtx q[4] = {
        {x0, y0, 0.0f, 1.0f, color, u0, v0},
        {x1, y0, 0.0f, 1.0f, color, u1, v0},
        {x0, y1, 0.0f, 1.0f, color, u0, v1},
        {x1, y1, 0.0f, 1.0f, color, u1, v1},
    };
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx));
}

// Called from Present, after the stereo composite and before the HUD
// texture is submitted: one draw per frame, on top of the game's HUD.
inline void Draw(IDirect3DDevice9* dev) {
    const auto& c = vrmod::config;
    if (view.hex == HEX_NONE && !view.ring)
        return;
    if (!stereo::HudLayerActive() || stereo::hud_rt == nullptr ||
        stereo::hud_width == 0)
        return;
    if (!EnsureAssets(dev))
        return;

    IDirect3DSurface9* saved_rt = nullptr;
    IDirect3DSurface9* saved_ds = nullptr;
    dev->GetRenderTarget(0, &saved_rt);
    dev->GetDepthStencilSurface(&saved_ds);
    D3DVIEWPORT9 saved_vp;
    dev->GetViewport(&saved_vp);
    DWORD saved_fvf = 0;
    dev->GetFVF(&saved_fvf);
    IDirect3DBaseTexture9* saved_tex = nullptr;
    dev->GetTexture(0, &saved_tex);
    const D3DRENDERSTATETYPE rs_ids[] = {
        D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
        D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLENDALPHA,
        D3DRS_DESTBLENDALPHA, D3DRS_ZENABLE, D3DRS_ZWRITEENABLE,
        D3DRS_CULLMODE, D3DRS_LIGHTING, D3DRS_FOGENABLE,
        D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE, D3DRS_BLENDOP};
    DWORD rs_saved[sizeof(rs_ids) / sizeof(rs_ids[0])];
    for (size_t i = 0; i < sizeof(rs_ids) / sizeof(rs_ids[0]); i++)
        dev->GetRenderState(rs_ids[i], &rs_saved[i]);
    const D3DTEXTURESTAGESTATETYPE ts_ids[] = {
        D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP,
        D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2, D3DTSS_TEXCOORDINDEX};
    DWORD ts_saved[sizeof(ts_ids) / sizeof(ts_ids[0])];
    for (size_t i = 0; i < sizeof(ts_ids) / sizeof(ts_ids[0]); i++)
        dev->GetTextureStageState(0, ts_ids[i], &ts_saved[i]);
    DWORD saved_stage1_color = 0, saved_stage1_alpha = 0;
    dev->GetTextureStageState(1, D3DTSS_COLOROP, &saved_stage1_color);
    dev->GetTextureStageState(1, D3DTSS_ALPHAOP, &saved_stage1_alpha);
    const D3DSAMPLERSTATETYPE ss_ids[] = {D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER,
                                         D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU,
                                         D3DSAMP_ADDRESSV};
    DWORD ss_saved[sizeof(ss_ids) / sizeof(ss_ids[0])];
    for (size_t i = 0; i < sizeof(ss_ids) / sizeof(ss_ids[0]); i++)
        dev->GetSamplerState(0, ss_ids[i], &ss_saved[i]);
    IDirect3DVertexShader9* saved_vs = nullptr;
    IDirect3DPixelShader9* saved_ps = nullptr;
    dev->GetVertexShader(&saved_vs);
    dev->GetPixelShader(&saved_ps);

    dev->BeginScene();
    stereo::BindHudLayer(dev, false, true);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(VTX_FVF);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    // Coverage into the HUD texture's alpha, as the game's own UI draws
    // get (stereo::ApplyCoverageAlpha): the quad is composited by alpha.
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    const float cx = c.swing_indicator_x, cy = c.swing_indicator_y;
    if (view.ring) {
        const int spin = c.swing_indicator_spin > 0 ? c.swing_indicator_spin : 1;
        const int frame = (int)((controller::GameTicks() / (unsigned long)spin) % RING_FRAMES);
        const float u0 = (frame % 4) * 0.25f, v0 = (frame / 4) * 0.25f;
        // The approach stops RING_HOVER_PX outside the end size; the
        // landing sits on it.
        const float end = c.swing_indicator_ring_end;
        const float hover = fminf(RING_HOVER_PX, fmaxf(0.0f, c.swing_indicator_ring - end));
        const float r = view.land ? end
                                  : end + hover + (c.swing_indicator_ring - end - hover) * view.ring_t;
        dev->SetTexture(0, tex_ring);
        Quad(dev, cx, cy, r, r, u0, v0, u0 + 0.25f, v0 + 0.25f, 0xFFFFFFFF);
    }
    if (view.hex != HEX_NONE) {
        const float s = c.swing_indicator_scale;
        const float u0 = (float)(view.hex * HEX_CELL_W) / HEX_ATLAS_W;
        const float u1 = (float)(view.hex * HEX_CELL_W + HEX_CELL_W) / HEX_ATLAS_W;
        const float v1 = (float)HEX_CELL_H / HEX_ATLAS_H;
        dev->SetTexture(0, tex_hex);
        Quad(dev, cx, cy, HEX_CELL_W * 0.5f * s, HEX_CELL_H * 0.5f * s, u0, 0.0f,
             u1, v1, 0xFFFFFFFF);
    }
    dev->EndScene();

    dev->SetVertexShader(saved_vs);
    dev->SetPixelShader(saved_ps);
    if (saved_vs) saved_vs->Release();
    if (saved_ps) saved_ps->Release();
    for (size_t i = 0; i < sizeof(ss_ids) / sizeof(ss_ids[0]); i++)
        dev->SetSamplerState(0, ss_ids[i], ss_saved[i]);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, saved_stage1_color);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, saved_stage1_alpha);
    for (size_t i = 0; i < sizeof(ts_ids) / sizeof(ts_ids[0]); i++)
        dev->SetTextureStageState(0, ts_ids[i], ts_saved[i]);
    for (size_t i = 0; i < sizeof(rs_ids) / sizeof(rs_ids[0]); i++)
        dev->SetRenderState(rs_ids[i], rs_saved[i]);
    dev->SetTexture(0, saved_tex);
    if (saved_tex) saved_tex->Release();
    dev->SetFVF(saved_fvf);
    dev->SetRenderTarget(0, saved_rt);
    dev->SetDepthStencilSurface(saved_ds);
    dev->SetViewport(&saved_vp);
    if (saved_rt) saved_rt->Release();
    if (saved_ds) saved_ds->Release();
}

}  // namespace swingind
