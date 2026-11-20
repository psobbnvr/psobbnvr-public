#pragma once

// The spell wheel's drawing side: the ring on VR panel slot kPanelWheel
// (the game's own technique and trap icons, names, counts, greying), and
// the chosen entry's icon on the palm of the left hand. Reads
// wheelui::state (psobbvr_wheel_ui.hpp) and wheel::Choice().
//
// The icons are the palette's own art, read from the HUD atlas
// f256_hyouji.prs in data\data.gsl with the swing indicator's loaders
// (psobbvr_swingind.hpp) and kept decoded in memory: the ring is drawn on
// the CPU, the hand icon from a texture made from the same texels. A cell
// is 50x43 texels; its corner comes from the exe's own tables, the ones
// the palette's icon mappers read.
//
// Greying follows the game: an icon the bar cannot use is drawn with the
// vertex colour 0xFF404040 (the bar update 0x71050C, after the usable test
// 0x68CE58). Here that grey marks what the character cannot use at all (a
// technique not learned, a trap on a non-android), and half brightness
// what it cannot use right now (the technique gate refuses: TP, a town,
// status; no traps left).
//
// The ring: a square straight-alpha bitmap drawn once per change (GDI for
// the text, the rest by hand) and shown on its own panel, carried rigidly
// with the right hand while it is open, faced toward the head when it
// opens. Panels are OpenXR only.
//
// The hand icon: a textured quad in the scene, drawn once per pass at the
// same point as the drawn hands (the first 2D draw after the scene, else
// EndScene) with its own gate, so it shows with IK arms and without a
// captured hand too. It rides the frame the left hand is drawn in
// (hands::DrawnHandFrame: the IK target or the replay's chain, the active
// grip tuple included), so it follows the hand when a gun's tuple moves
// it. Depth-tested, so the hand and the weapon hide it, unless [wheel]
// hand_icon_depth_test is 0.
//
// Threads: Update runs at BeginScene after the controller and the draws
// inside the device's draw calls, all on the game's own thread (see
// psobbvr_wheel.hpp), so the game routines called here (the technique
// gate, the name getter) never race the game.

#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "psobbvr_gamecam.hpp"
#include "psobbvr_hands.hpp"
#include "psobbvr_log.hpp"
#include "psobbvr_resolution.hpp"
#include "psobbvr_stereo.hpp"
#include "psobbvr_swingind.hpp"
#include "psobbvr_vr.hpp"
#include "psobbvr_wheel.hpp"
#include "psobbvr_wheel_char.hpp"
#include "psobbvr_wheel_config.hpp"
#include "psobbvr_wheel_core.hpp"
#include "psobbvr_wheel_ui.hpp"

namespace wheeldraw {

using wheelcore::Entry;
using wheelcore::Kind;
using wheelcore::kSegments;

// ---- the game's icon art ----------------------------------------------------

// The HUD atlas and the two of its textures the palette icons come from
// (entries 5 and 7 of the HUD texture list 0x9F58C8 that 0x712D14 loads):
// f256_actionpallet_b holds the techniques, f256_duelcount the traps and
// the empty-slot cell. 256x256, XVRT format 7 (DXT3 block layout).
constexpr const char* ATLAS_ENTRY = "f256_hyouji.prs";
constexpr uint32_t TEX_ID_TECHS = 0x0B78EB;
constexpr uint32_t TEX_ID_TRAPS = 0x0C0292;
constexpr unsigned TEX_SIZE = 256;
constexpr uint32_t XVR_FORMAT_DXT3 = 7;
enum ArtTexture : int { ART_TECHS = 0, ART_TRAPS = 1, ART_COUNT = 2 };
// An icon cell in texels (the icon mappers' constants 0x96F55C / 0x96F558).
constexpr int CELL_W = 50, CELL_H = 43;
// The icon mappers 0x70FF56 (bar and palette) and 0x710288 (customize
// list) take a cell's top-left corner from tables of (float x, float y), 8
// bytes per parameter: techniques by id (palette type 3)...
constexpr uintptr_t TECH_CELL_TABLE_ADDR = 0x009F12E0;
// ...and items by parameter (type 4), where parameters 13..16 are the
// traps.
constexpr uintptr_t ITEM_CELL_TABLE_ADDR = 0x009F1380;
// An empty slot (type 0): the bare hexagon backing at (100, 0) of the
// traps' texture (the mappers' constants 0x96F564 / 0x96F560).
constexpr int EMPTY_CELL_X = 100, EMPTY_CELL_Y = 0;

// ---- names ------------------------------------------------------------------

// The technique-name getter: cdecl (id), the UTF-16 name in the client's
// language, read as [[[0xA9CD50] + 0x14] + id*4] (text collection 5 of
// unitxt_<lang>.prs). No checks of its own.
constexpr uintptr_t TECH_NAME_ADDR = 0x00793000;
// The loaded text collections: a pointer to the collection pointers, 0
// until the text loader near 0x793300 has run.
constexpr uintptr_t TEXT_COLLECTIONS_ADDR = 0x00A9CD50;
constexpr uintptr_t TECH_NAME_COLLECTION_OFF = 0x14;  // collection 5
using TechNameFn = const wchar_t*(__cdecl*)(int id);

constexpr int NAME_LEN = 32;
inline wchar_t tech_names[wheelcore::kTechCount][NAME_LEN];
inline bool tech_name_from_game[wheelcore::kTechCount];
inline wchar_t trap_names[wheelcore::kTrapCount][NAME_LEN];
inline bool names_init = false;
inline unsigned names_version = 0;  // +1 whenever a name changes (a redraw)

inline void CopyAscii(wchar_t* dst, const char* src) {
    int i = 0;
    for (; i < NAME_LEN - 1 && src[i] != '\0'; i++)
        dst[i] = (wchar_t)(unsigned char)src[i];
    dst[i] = 0;
}

// The English names first, then the game's own once its text is loaded;
// a name the game did not give is asked for again on the next call.
inline void EnsureNames() {
    if (!names_init) {
        names_init = true;
        for (int i = 0; i < wheelcore::kTechCount; i++)
            CopyAscii(tech_names[i], wheelcore::kTechs[i].name);
        for (int i = 0; i < wheelcore::kTrapCount; i++)
            CopyAscii(trap_names[i], wheelcore::kTraps[i].name);
        names_version++;
    }
    bool missing = false;
    for (int i = 0; i < wheelcore::kTechCount; i++)
        missing = missing || !tech_name_from_game[i];
    if (!missing)
        return;
    const uintptr_t sets = *reinterpret_cast<const volatile uintptr_t*>(TEXT_COLLECTIONS_ADDR);
    if (sets == 0 || !diag::Accessible(sets + TECH_NAME_COLLECTION_OFF, 4, false))
        return;
    const uintptr_t coll = *reinterpret_cast<const uintptr_t*>(sets + TECH_NAME_COLLECTION_OFF);
    if (coll == 0 || !diag::Accessible(coll, 4 * wheelcore::kTechCount, false))
        return;
    const TechNameFn get = reinterpret_cast<TechNameFn>(TECH_NAME_ADDR);
    for (int i = 0; i < wheelcore::kTechCount; i++) {
        if (tech_name_from_game[i])
            continue;
        const wchar_t* s = get(i);
        if (s == nullptr || !diag::Accessible(reinterpret_cast<uintptr_t>(s), 2, false))
            continue;
        wchar_t name[NAME_LEN];
        int n = 0;
        for (; n < NAME_LEN - 1; n++) {
            const uintptr_t a = reinterpret_cast<uintptr_t>(s + n);
            if ((a & 0xFFF) == 0 && !diag::Accessible(a, 2, false))
                break;
            if (s[n] == 0)
                break;
            name[n] = s[n];
        }
        name[n] = 0;
        if (n == 0)
            continue;
        if (wcscmp(name, tech_names[i]) != 0) {
            wcscpy_s(tech_names[i], name);
            names_version++;
        }
        tech_name_from_game[i] = true;
    }
}

inline const wchar_t* DisplayName(Entry e) {
    e = wheelcore::Clamped(e);
    EnsureNames();
    switch (e.kind) {
    case Kind::Technique: return tech_names[e.index];
    case Kind::Trap: return trap_names[e.index];
    default: return nullptr;
    }
}

// ---- icon art ------------------------------------------------------------------

inline int art_state = 0;  // 0 untried, 1 loaded, -1 failed (logged once)
inline std::vector<uint32_t> art_px[ART_COUNT];  // 0xAARRGGBB, 256x256 each
inline IDirect3DTexture9* art_tex[ART_COUNT] = {};  // the hand icon's; DEFAULT pool
inline bool tex_failed = false;  // creation failed since the last reset (logged)

inline bool EnsureArt() {
    if (art_state != 0)
        return art_state == 1;
    art_state = -1;
    std::vector<uint8_t> prs, xvm;
    if (!swingind::ReadGslEntry(ATLAS_ENTRY, prs)) {
        diag::Log("wheel: icons unavailable (%s not read from data.gsl) - names only",
                  ATLAS_ENTRY);
        return false;
    }
    if (!swingind::PrsDecompress(prs, xvm)) {
        diag::Log("wheel: icons unavailable (%s did not decompress) - names only", ATLAS_ENTRY);
        return false;
    }
    const uint32_t ids[ART_COUNT] = {TEX_ID_TECHS, TEX_ID_TRAPS};
    for (int t = 0; t < ART_COUNT; t++) {
        const uint8_t* data = nullptr;
        uint32_t fmt = 0, size = 0;
        unsigned w = 0, h = 0;
        const bool found = swingind::FindXvr(xvm, ids[t], data, fmt, w, h, size);
        if (!found || fmt != XVR_FORMAT_DXT3 || w != TEX_SIZE || h != TEX_SIZE ||
            !swingind::DecodeDxt3(data, size, w, h, art_px[t])) {
            diag::Log("wheel: icons unavailable (texture 0x%06X in %s: %s, format %u, %ux%u) - "
                      "names only",
                      ids[t], ATLAS_ENTRY, found ? "found" : "missing", fmt, w, h);
            for (int k = 0; k < ART_COUNT; k++)
                art_px[k].clear();
            return false;
        }
    }
    art_state = 1;
    diag::Log("wheel: icons loaded from %s (techniques 0x%06X, traps 0x%06X)", ATLAS_ENTRY,
              TEX_ID_TECHS, TEX_ID_TRAPS);
    return true;
}

// The hand icon's textures, made lazily from the kept texels.
inline bool EnsureTextures(IDirect3DDevice9* dev) {
    if (art_tex[ART_TECHS] != nullptr && art_tex[ART_TRAPS] != nullptr)
        return true;
    if (tex_failed || !EnsureArt())
        return false;
    for (int t = 0; t < ART_COUNT; t++)
        if (art_tex[t] == nullptr)
            art_tex[t] = swingind::MakeTexture(dev, TEX_SIZE, TEX_SIZE, art_px[t].data());
    if (art_tex[ART_TECHS] == nullptr || art_tex[ART_TRAPS] == nullptr) {
        diag::Log("wheel: hand icon texture creation failed - no hand icon until the next "
                  "device reset");
        tex_failed = true;
        for (int t = 0; t < ART_COUNT; t++)
            if (art_tex[t] != nullptr) {
                art_tex[t]->Release();
                art_tex[t] = nullptr;
            }
        return false;
    }
    return true;
}

// Device reset or teardown: the textures go (they are recreated on the
// next draw); the decoded texels stay.
inline void ReleaseAssets() {
    for (int t = 0; t < ART_COUNT; t++)
        if (art_tex[t] != nullptr) {
            art_tex[t]->Release();
            art_tex[t] = nullptr;
        }
    tex_failed = false;
}

// The texture and the cell's top-left texel for an entry, from the game's
// tables (read in place: the exe is never relocated). An empty entry is
// the empty-slot cell. False when the table holds no usable cell.
inline bool CellOf(Entry e, int& tex, int& x, int& y) {
    e = wheelcore::Clamped(e);
    float fx = 0.0f, fy = 0.0f;
    switch (e.kind) {
    case Kind::Technique: {
        const float* p = reinterpret_cast<const float*>(TECH_CELL_TABLE_ADDR + 8u * (uintptr_t)e.index);
        fx = p[0];
        fy = p[1];
        tex = ART_TECHS;
        break;
    }
    case Kind::Trap: {
        const float* p = reinterpret_cast<const float*>(
            ITEM_CELL_TABLE_ADDR + 8u * (uintptr_t)(wheel::TRAP_PARAM_BASE + (uint32_t)e.index));
        fx = p[0];
        fy = p[1];
        tex = ART_TRAPS;
        break;
    }
    default:
        tex = ART_TRAPS;
        x = EMPTY_CELL_X;
        y = EMPTY_CELL_Y;
        return true;
    }
    if (!(fx >= 0.0f && fy >= 0.0f && fx <= (float)(TEX_SIZE - CELL_W) &&
          fy <= (float)(TEX_SIZE - CELL_H)))
        return false;
    x = (int)(fx + 0.5f);
    y = (int)(fy + 0.5f);
    return true;
}

// ---- availability -----------------------------------------------------------

enum Avail : uint8_t { AVAIL_FULL = 0, AVAIL_HALF = 1, AVAIL_GREY = 2 };
// The colour each multiplies the icon by; the grey is the game's own.
constexpr uint32_t kAvailColour[3] = {0xFFFFFFFFu, 0xFF808080u, 0xFF404040u};
constexpr const char* kAvailName[3] = {"full", "half (not now)", "grey (cannot use)"};
// A learned-level byte for a technique not learned.
constexpr uint8_t TECH_NOT_LEARNED = 0xFF;
// The bar shows a trap count clamped to 0..20 (0x70FE27).
constexpr int TRAP_COUNT_MAX = 20;

struct Status {
    uint8_t avail = AVAIL_FULL;
    int8_t count = -1;  // a trap's count to show, -1 none
};
inline bool operator==(const Status& a, const Status& b) {
    return a.avail == b.avail && a.count == b.count;
}

// The local player's entity, readable through the fields used here and by
// the gate's own reads; 0 without one.
inline uintptr_t ReadableEntity() {
    const uintptr_t e = wheel::LocalEntity();
    if (e == 0 || !diag::Accessible(e, wheel::ENT_RACE_FLAGS_OFF + 4, false))
        return 0;
    return e;
}

// The game's technique gate, asked at most once per technique per
// presented frame (wheel::frame_serial; BeginScene runs twice per frame).
inline unsigned gate_frame[wheelcore::kTechCount];
inline uintptr_t gate_entity[wheelcore::kTechCount];
inline bool gate_cached[wheelcore::kTechCount];
inline bool gate_value[wheelcore::kTechCount];
inline unsigned gate_calls = 0;

inline bool GateNow(uintptr_t entity, int id) {
    if (gate_cached[id] && gate_frame[id] == wheel::frame_serial && gate_entity[id] == entity)
        return gate_value[id];
    const int r = reinterpret_cast<wheel::GateFn>(wheel::TECH_GATE_ADDR)(entity, 0, id);
    gate_cached[id] = true;
    gate_frame[id] = wheel::frame_serial;
    gate_entity[id] = entity;
    gate_value[id] = r != 0;
    gate_calls++;
    return gate_value[id];
}

// What an entry looks like for the character: no entity = full.
inline Status StatusOf(Entry e, uintptr_t entity) {
    Status s;
    e = wheelcore::Clamped(e);
    if (entity == 0 || e.kind == Kind::None)
        return s;
    if (e.kind == Kind::Technique) {
        const uint8_t lv =
            *reinterpret_cast<const uint8_t*>(entity + wheel::ENT_TECH_LEVELS_OFF + (uintptr_t)e.index);
        if (lv == TECH_NOT_LEARNED)
            s.avail = AVAIL_GREY;
        else if (!GateNow(entity, e.index))
            s.avail = AVAIL_HALF;
        return s;
    }
    const bool android =
        (*reinterpret_cast<const uint32_t*>(entity + wheel::ENT_RACE_FLAGS_OFF) &
         wheel::ENT_ANDROID_BIT) != 0;
    if (!android) {
        s.avail = AVAIL_GREY;
        return s;
    }
    int c = *reinterpret_cast<const int8_t*>(entity + wheel::ENT_TRAP_COUNTS_OFF + (uintptr_t)e.index);
    c = c < 0 ? 0 : c > TRAP_COUNT_MAX ? TRAP_COUNT_MAX : c;
    s.count = (int8_t)c;
    if (c == 0)
        s.avail = AVAIL_HALF;
    return s;
}

// ---- what the ring shows ------------------------------------------------------

struct View {
    int page = 0;
    int page_count = 0;  // 0 = nothing set up: every segment empty, the message
    int highlight = -1;  // 0..7, -1 none
    Entry seg[kSegments];
};

struct Model {
    View view;
    Status status[kSegments];
    unsigned names = 0;  // names_version
};

inline bool operator==(const Model& a, const Model& b) {
    if (a.view.page != b.view.page || a.view.page_count != b.view.page_count ||
        a.view.highlight != b.view.highlight || a.names != b.names)
        return false;
    for (int i = 0; i < kSegments; i++)
        if (a.view.seg[i] != b.view.seg[i] || !(a.status[i] == b.status[i]))
            return false;
    return true;
}

inline View ViewOf(const wheelui::State& s) {
    View v;
    v.page_count = s.page_count > 0 ? s.page_count : 0;
    v.page = s.page;
    v.highlight = s.highlight >= 0 && s.highlight < kSegments ? s.highlight : -1;
    for (int i = 0; i < kSegments; i++)
        v.seg[i] = v.page_count > 0 ? wheelcore::Clamped(s.segments[i]) : Entry();
    return v;
}

// A page of the wheel of the character in play, whose class byte is `cls`
// (an empty ring when it has no pages, or they are another class's).
inline View LayoutView(int cls, int page, int highlight) {
    View v;
    v.highlight = highlight >= 0 && highlight < kSegments ? highlight : -1;
    if (cls < 0 || cls >= wheelcore::kClassCount)
        return v;
    const wheelcore::Layout* lay = wheelchar::LayoutFor(cls);
    if (lay == nullptr)
        return v;
    v.page_count = wheelcore::ClampInt(lay->pages, 0, wheelcore::kMaxPages);
    if (v.page_count == 0)
        return v;
    v.page = wheelcore::ClampInt(page, 0, v.page_count - 1);
    for (int i = 0; i < kSegments; i++)
        v.seg[i] = wheelcore::Clamped(lay->page[v.page].seg[i]);
    return v;
}

inline int LocalClass(uintptr_t entity) {
    if (entity == 0)
        return -1;
    const int c = *reinterpret_cast<const uint8_t*>(entity + wheel::ENT_CLASS_OFF);
    return c < wheelcore::kClassCount ? c : -1;
}

inline Model BuildModel(const View& v, uintptr_t entity) {
    Model m;
    m.view = v;
    for (int i = 0; i < kSegments; i++)
        m.status[i] = StatusOf(v.seg[i], entity);
    EnsureNames();
    m.names = names_version;
    return m;
}

// ---- the ring's bitmap ----------------------------------------------------------

constexpr int PANEL_PX = 768;  // square
constexpr int PANEL_SLOT = vrmod::VRInterface::kPanelWheel;
// Geometry in pixels about the centre: the disc's edge and its rim, the
// hub's edge and its rim, the dividers' half-width, and the lit segment's
// rim and the glow that fades inward from it.
constexpr float CENTRE_PX = PANEL_PX * 0.5f;
constexpr float R_DISC = 378.0f;
constexpr float OUTER_RIM_PX = 3.0f;
constexpr float R_HUB = 140.0f;
constexpr float HUB_RIM_PX = 3.0f;
constexpr float DIVIDER_HALF_PX = 1.0f;
constexpr float LIT_RIM_PX = 4.0f;
constexpr float LIT_GLOW_PX = 12.0f;
constexpr float LIT_GLOW_MAX = 0.5f;  // the glow's strength at the lit rim
constexpr float LIT_TINT = 0.6f;      // how far the lit fill leans toward COL_TEAL_LIT
constexpr float R_ICON = (R_HUB + R_DISC - OUTER_RIM_PX) * 0.5f;  // where a segment's icon sits
constexpr int ICON_SCALE = 2;         // the 50x43 cell drawn 100x86
constexpr int ICON_LIFT = 14;         // icon above the segment centre, room for the name
constexpr uint8_t PANEL_ALPHA = 225;  // the HUD panel body's; the cyan lines are opaque
constexpr int HUB_REGION = kSegments; // region id of the hub
// Colours from the game's own HUD art (the palette windows, the slot plate
// 0x0C0E27 and the HUD hexagons in f256_hyouji.prs): the panel body's
// near-black teal and its scanlines, the darker plate the hub uses, the
// cyan of the slot plate's rim and every HUD hexagon's outline, and the
// brighter panel teal a lit segment leans toward.
constexpr COLORREF COL_BODY = RGB(0, 16, 24);
constexpr COLORREF COL_BODY_LINE = RGB(0, 52, 49);
constexpr COLORREF COL_HUB = RGB(0, 0, 8);
constexpr COLORREF COL_HUB_LINE = RGB(0, 34, 36);
constexpr COLORREF COL_RIM = RGB(82, 239, 255);
constexpr COLORREF COL_TEAL_LIT = RGB(0, 102, 117);
constexpr COLORREF COL_HUB_TEXT = RGB(200, 245, 255);
constexpr COLORREF COL_OUTLINE = RGB(8, 10, 16);
// The scanlines: a 6-row period, two full rows with a half-strength row on
// each side and two empty, so the pattern keeps the HUD's look without
// shimmering once the compositor resamples the panel.
constexpr int SCAN_PERIOD = 6;
constexpr float kScan[SCAN_PERIOD] = {0.5f, 1.0f, 1.0f, 0.5f, 0.0f, 0.0f};

inline void* dib_bits = nullptr;
inline HBITMAP dib = nullptr;
inline HDC dib_dc = nullptr;
inline HFONT font_name = nullptr, font_count = nullptr, font_page = nullptr, font_msg = nullptr;
inline bool dib_failed = false;

// Per pixel: the nearest region (segment 0..7 by angle, or the hub), the
// distance into a segment from its cyan lines (1/8 px, clamped), the
// lines' coverage (0..255) and the unlit picture with its alpha; each
// segment's bounding box. Built once.
inline std::vector<uint8_t> geo_region;
inline std::vector<int8_t> geo_edge;
inline std::vector<uint8_t> geo_line;
inline std::vector<uint32_t> base_px;
inline RECT seg_box[kSegments];

inline uint32_t Pack(float r, float g, float b, unsigned a) {
    auto c = [](float v) { return (uint32_t)(v < 0.0f ? 0.0f : v > 255.0f ? 255.0f : v + 0.5f); };
    return ((uint32_t)a << 24) | (c(r) << 16) | (c(g) << 8) | c(b);
}

inline float Mix(float a, float b, float t) { return a + (b - a) * t; }
inline float Cov(float d) { return d + 0.5f < 0.0f ? 0.0f : d + 0.5f > 1.0f ? 1.0f : d + 0.5f; }

struct Rgb {
    float r, g, b;
};
inline Rgb ToRgb(COLORREF c) {
    return Rgb{(float)GetRValue(c), (float)GetGValue(c), (float)GetBValue(c)};
}
inline Rgb MixRgb(Rgb a, Rgb b, float t) {
    return Rgb{Mix(a.r, b.r, t), Mix(a.g, b.g, t), Mix(a.b, b.b, t)};
}

// A region's scanlined fill on row py, lit or not.
inline Rgb Fill(int region, int py, bool lit) {
    const float s = kScan[py % SCAN_PERIOD];
    if (region == HUB_REGION)
        return MixRgb(ToRgb(COL_HUB), ToRgb(COL_HUB_LINE), s);
    const Rgb c = MixRgb(ToRgb(COL_BODY), ToRgb(COL_BODY_LINE), s);
    return lit ? MixRgb(c, ToRgb(COL_TEAL_LIT), LIT_TINT) : c;
}

// One pixel's colour (alpha left out): the fill, for the lit segment its
// rim and glow (e = distance in from its lines), then the cyan lines at
// their coverage.
inline uint32_t ShadePixel(int region, int py, float e, float line, bool lit) {
    Rgb c = Fill(region, py, lit);
    if (lit && region != HUB_REGION) {
        const float rim = Cov(LIT_RIM_PX - e);
        float t = (e - LIT_RIM_PX) / LIT_GLOW_PX;
        t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
        const float glow = LIT_GLOW_MAX * (1.0f - t) * (1.0f - t);
        c = MixRgb(c, ToRgb(COL_RIM), fmaxf(rim, glow));
    }
    c = MixRgb(c, ToRgb(COL_RIM), line);
    return Pack(c.r, c.g, c.b, 0);
}

inline void BuildGeometry() {
    if (!base_px.empty())
        return;
    const size_t n = (size_t)PANEL_PX * PANEL_PX;
    geo_region.assign(n, 0);
    geo_edge.assign(n, 0);
    geo_line.assign(n, 0);
    base_px.assign(n, 0);
    for (int i = 0; i < kSegments; i++)
        seg_box[i] = RECT{PANEL_PX, PANEL_PX, 0, 0};
    const float pi = 3.14159265f;
    // Segment centres and boundaries, clockwise from the top (x right, y up).
    float cx[kSegments], cy[kSegments], bx[kSegments], by[kSegments];
    for (int i = 0; i < kSegments; i++) {
        const float a = i * pi / 4.0f;
        cx[i] = sinf(a);
        cy[i] = cosf(a);
        const float ab = a - pi / 8.0f;  // the boundary before segment i
        bx[i] = sinf(ab);
        by[i] = cosf(ab);
    }
    const float r_rim_in = R_DISC - OUTER_RIM_PX;
    for (int py = 0; py < PANEL_PX; py++) {
        for (int px = 0; px < PANEL_PX; px++) {
            const size_t idx = (size_t)py * PANEL_PX + px;
            const float x = px + 0.5f - CENTRE_PX, y = CENTRE_PX - (py + 0.5f);
            const float r = sqrtf(x * x + y * y);
            const float disc = Cov(R_DISC - r);
            if (disc <= 0.0f)
                continue;  // outside: transparent
            int region = HUB_REGION;
            float e = 0.0f, divider = 0.0f;
            // Split at the middle of the hub's rim, so each edge of the rim
            // blends into the fill beside it (no dark seam).
            if (r >= R_HUB - HUB_RIM_PX * 0.5f) {
                region = 0;
                float best = -2.0f;
                for (int i = 0; i < kSegments; i++) {
                    const float dot = x * cx[i] + y * cy[i];
                    if (dot > best) {
                        best = dot;
                        region = i;
                    }
                }
                // Distance to the nearer of its two boundary lines through
                // the centre, where the dividers run.
                const int nxt = (region + 1) % kSegments;
                const float d0 = fabsf(x * by[region] - y * bx[region]);
                const float d1 = fabsf(x * by[nxt] - y * bx[nxt]);
                const float d_div = fminf(d0, d1);
                divider = Cov(DIVIDER_HALF_PX - d_div);
                e = fminf(fminf(d_div - DIVIDER_HALF_PX, r - R_HUB), r_rim_in - r);
                if (e > -2.0f) {
                    RECT& bb = seg_box[region];
                    if (px < bb.left) bb.left = px;
                    if (py < bb.top) bb.top = py;
                    if (px + 1 > bb.right) bb.right = px + 1;
                    if (py + 1 > bb.bottom) bb.bottom = py + 1;
                }
            }
            const float hub_rim = fminf(Cov(r - (R_HUB - HUB_RIM_PX)), Cov(R_HUB - r));
            const float outer_rim = Cov(r - r_rim_in);
            const float line = fmaxf(divider, fmaxf(hub_rim, outer_rim));
            const unsigned alpha =
                (unsigned)(disc * (PANEL_ALPHA + (255.0f - PANEL_ALPHA) * line) + 0.5f);
            const float q = e * 8.0f;
            geo_region[idx] = (uint8_t)region;
            geo_edge[idx] = (int8_t)(q < -128.0f ? -128 : q > 127.0f ? 127 : (int)lroundf(q));
            geo_line[idx] = (uint8_t)(line * 255.0f + 0.5f);
            base_px[idx] = alpha == 0 ? 0u
                                      : (ShadePixel(region, py, e, line, false) |
                                         ((uint32_t)alpha << 24));
        }
    }
}

inline HFONT MakeFont(int px, int weight) {
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

inline bool EnsureDib() {
    if (dib != nullptr)
        return true;
    if (dib_failed)
        return false;
    dib_dc = CreateCompatibleDC(nullptr);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = PANEL_PX;
    bmi.bmiHeader.biHeight = -PANEL_PX;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    if (dib_dc != nullptr)
        dib = CreateDIBSection(dib_dc, &bmi, DIB_RGB_COLORS, &dib_bits, nullptr, 0);
    if (dib == nullptr || dib_bits == nullptr) {
        if (dib_dc != nullptr)
            DeleteDC(dib_dc);
        dib_dc = nullptr;
        dib = nullptr;
        dib_bits = nullptr;
        dib_failed = true;
        diag::Log("wheel: ring bitmap creation failed - no ring");
        return false;
    }
    SelectObject(dib_dc, dib);
    font_name = MakeFont(26, FW_SEMIBOLD);
    font_count = MakeFont(28, FW_BOLD);
    font_page = MakeFont(30, FW_SEMIBOLD);
    font_msg = MakeFont(22, FW_SEMIBOLD);
    return true;
}

// One cell blitted at ICON_SCALE, centred at (cx, cy): bilinear on
// premultiplied texels (clamped to the cell, so no neighbour bleeds in),
// multiplied by `mult`, over what is there.
inline void BlitIcon(uint32_t* dst, int cx, int cy, const std::vector<uint32_t>& src, int sx0,
                     int sy0, uint32_t mult) {
    const int w = CELL_W * ICON_SCALE, h = CELL_H * ICON_SCALE;
    const int x0 = cx - w / 2, y0 = cy - h / 2;
    const float mr = ((mult >> 16) & 255) / 255.0f, mg = ((mult >> 8) & 255) / 255.0f,
                mb = (mult & 255) / 255.0f, ma = ((mult >> 24) & 255) / 255.0f;
    for (int dy = 0; dy < h; dy++) {
        const int py = y0 + dy;
        if (py < 0 || py >= PANEL_PX)
            continue;
        float fy = (dy + 0.5f) / ICON_SCALE - 0.5f;
        fy = fy < 0.0f ? 0.0f : fy > CELL_H - 1.0f ? CELL_H - 1.0f : fy;
        const int iy0 = (int)fy;
        const int iy1 = iy0 + 1 < CELL_H ? iy0 + 1 : iy0;
        const float ty = fy - iy0;
        for (int dx = 0; dx < w; dx++) {
            const int px = x0 + dx;
            if (px < 0 || px >= PANEL_PX)
                continue;
            float fx = (dx + 0.5f) / ICON_SCALE - 0.5f;
            fx = fx < 0.0f ? 0.0f : fx > CELL_W - 1.0f ? CELL_W - 1.0f : fx;
            const int ix0 = (int)fx;
            const int ix1 = ix0 + 1 < CELL_W ? ix0 + 1 : ix0;
            const float tx = fx - ix0;
            const uint32_t t[4] = {
                src[(size_t)(sy0 + iy0) * TEX_SIZE + sx0 + ix0],
                src[(size_t)(sy0 + iy0) * TEX_SIZE + sx0 + ix1],
                src[(size_t)(sy0 + iy1) * TEX_SIZE + sx0 + ix0],
                src[(size_t)(sy0 + iy1) * TEX_SIZE + sx0 + ix1]};
            const float wgt[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
            float a = 0.0f, r = 0.0f, g = 0.0f, b = 0.0f;
            for (int k = 0; k < 4; k++) {
                const float ka = ((t[k] >> 24) & 255) / 255.0f * wgt[k];
                a += ka;
                r += ((t[k] >> 16) & 255) * ka;
                g += ((t[k] >> 8) & 255) * ka;
                b += (t[k] & 255) * ka;
            }
            a *= ma;
            if (a <= 0.0f)
                continue;
            uint32_t& d = dst[(size_t)py * PANEL_PX + px];
            const float keep = 1.0f - a;
            d = Pack(r * mr * ma + ((d >> 16) & 255) * keep, g * mg * ma + ((d >> 8) & 255) * keep,
                     b * mb * ma + (d & 255) * keep, 0);
        }
    }
}

// Text with a dark outline, so it reads over any colour. The rectangle
// only places it (descenders may pass its edge).
inline void OutlinedText(HDC dc, const wchar_t* s, RECT r, UINT fmt, COLORREF ink) {
    static const int kOff[8][2] = {{-2, 0}, {2, 0}, {0, -2}, {0, 2},
                                   {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    fmt |= DT_NOPREFIX | DT_SINGLELINE | DT_NOCLIP;
    SetTextColor(dc, COL_OUTLINE);
    for (const auto& o : kOff) {
        RECT ro = {r.left + o[0], r.top + o[1], r.right + o[0], r.bottom + o[1]};
        DrawTextW(dc, s, -1, &ro, fmt);
    }
    SetTextColor(dc, ink);
    DrawTextW(dc, s, -1, &r, fmt);
}

inline unsigned rasters = 0;

// The whole picture for a model into the DIB; false without one.
inline bool Rasterize(const Model& m) {
    if (!EnsureDib())
        return false;
    BuildGeometry();
    const bool art = EnsureArt();
    uint32_t* px = static_cast<uint32_t*>(dib_bits);
    GdiFlush();
    memcpy(px, base_px.data(), base_px.size() * sizeof(uint32_t));
    // The lit segment.
    const int hl = m.view.highlight;
    if (hl >= 0 && hl < kSegments) {
        const RECT& bb = seg_box[hl];
        for (int y = bb.top; y < bb.bottom; y++)
            for (int x = bb.left; x < bb.right; x++) {
                const size_t i = (size_t)y * PANEL_PX + x;
                if (geo_region[i] != hl || base_px[i] == 0)
                    continue;
                px[i] = ShadePixel(hl, y, geo_edge[i] / 8.0f, geo_line[i] / 255.0f, true);
            }
    }
    // Icons, then the text over them.
    int seg_x[kSegments], seg_y[kSegments];
    for (int i = 0; i < kSegments; i++) {
        const float a = i * 3.14159265f / 4.0f;
        seg_x[i] = (int)lroundf(CENTRE_PX + R_ICON * sinf(a));
        seg_y[i] = (int)lroundf(CENTRE_PX - R_ICON * cosf(a));
    }
    if (art) {
        for (int i = 0; i < kSegments; i++) {
            const Entry e = m.view.seg[i];
            int tex = 0, cx = 0, cy = 0;
            if (!CellOf(e, tex, cx, cy))
                continue;
            const uint32_t mult =
                e.kind == Kind::None ? kAvailColour[AVAIL_HALF] : kAvailColour[m.status[i].avail];
            BlitIcon(px, seg_x[i], seg_y[i] - ICON_LIFT, art_px[tex], cx, cy, mult);
        }
    }
    HDC dc = dib_dc;
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old_font = SelectObject(dc, font_name);
    const int icon_half_h = CELL_H * ICON_SCALE / 2;
    for (int i = 0; i < kSegments; i++) {
        const Entry e = m.view.seg[i];
        const wchar_t* name = e.kind == Kind::None ? nullptr : DisplayName(e);
        if (name == nullptr)
            continue;
        static const COLORREF kInk[3] = {RGB(255, 255, 255), RGB(200, 200, 200),
                                         RGB(150, 150, 150)};
        const COLORREF ink = kInk[m.status[i].avail];
        // Under the icon, or at the segment's centre without icons.
        const int top = art ? seg_y[i] - ICON_LIFT + icon_half_h + 2 : seg_y[i] - 16;
        SelectObject(dc, font_name);
        OutlinedText(dc, name, RECT{seg_x[i] - 100, top, seg_x[i] + 100, top + 32},
                     DT_CENTER | DT_TOP, ink);
        if (m.status[i].count >= 0) {
            wchar_t num[8];
            swprintf_s(num, L"%d", (int)m.status[i].count);
            const int right = seg_x[i] + CELL_W * ICON_SCALE / 2 + 6;
            const int bottom = art ? seg_y[i] - ICON_LIFT + icon_half_h + 4 : seg_y[i] - 16;
            SelectObject(dc, font_count);
            OutlinedText(dc, num, RECT{right - 70, bottom - 34, right, bottom},
                         DT_RIGHT | DT_BOTTOM, RGB(255, 255, 255));
        }
    }
    const int cx = (int)CENTRE_PX, cy = (int)CENTRE_PX;
    if (m.view.page_count == 0) {
        SelectObject(dc, font_msg);
        OutlinedText(dc, L"Set up the wheel", RECT{cx - 140, cy - 34, cx + 140, cy - 2},
                     DT_CENTER | DT_BOTTOM, COL_HUB_TEXT);
        OutlinedText(dc, L"in the Options program", RECT{cx - 140, cy + 2, cx + 140, cy + 34},
                     DT_CENTER | DT_TOP, COL_HUB_TEXT);
    } else if (m.view.page_count > 1) {
        wchar_t text[32];
        swprintf_s(text, L"page %d / %d", m.view.page + 1, m.view.page_count);
        SelectObject(dc, font_page);
        OutlinedText(dc, text, RECT{cx - 130, cy - 20, cx + 130, cy + 20},
                     DT_CENTER | DT_VCENTER, COL_HUB_TEXT);
    }
    SelectObject(dc, old_font);
    GdiFlush();
    // Alpha: GDI zeroes the byte it paints; every pixel takes the disc's.
    const size_t n = base_px.size();
    for (size_t i = 0; i < n; i++) {
        const uint32_t a = base_px[i] & 0xFF000000u;
        px[i] = a == 0 ? 0u : ((px[i] & 0x00FFFFFFu) | a);
    }
    rasters++;
    return true;
}

// ---- the ring's panel ---------------------------------------------------------

// Placement. The ring rides the RIGHT hand, the one whose stick picks, so
// the player is already looking there. Its pose is set once, when it
// opens: worked out in tracking space - [wheel] ring_raise_m above the
// right grip, moved ring_toward_head_m toward the head, turned to face
// the head at that moment - and handed to the backend relative to the
// right grip's own pose, with the slot in that grip's space
// (VRInterface::SetPanelSpace). The compositor then places it from the
// controller's latest pose at every display refresh, so it stays on the
// hand and turns with it, like a watch face; a pose held in tracking
// space would step behind the hand at the game's 30 Hz. It is not
// re-faced each game frame: the ring would turn with the hand between
// game frames and snap back at each one, a judder. Without a right-hand
// pose at open it stays fixed in tracking space, ahead of the head and
// below eye level.
constexpr int RING_HAND = 1;                  // right
constexpr float RING_MIN_HEAD_GAP_M = 0.05f;  // closer than this, no move toward the head
constexpr float RING_FALLBACK_AHEAD_M = 0.5f;
constexpr float RING_FALLBACK_DROP_M = 0.2f;
// A panel the backend is not showing (it was not ready, or failed to make
// its resources) is offered again this often, in presented frames.
constexpr unsigned RETRY_FRAMES = 30;
// An open serial no real open has; a ring shown with it is logged as forced.
constexpr unsigned FORCED_SERIAL = 0xFFFFFFFFu;

inline bool panel_open = false;   // the ring is up (ours)
inline unsigned shown_serial = 0; // the open it was placed for
inline int panel_space = -1;      // -1 tracking space, RING_HAND the right grip's
inline D3DMATRIX panel_pose = {}; // as given to the backend, in panel_space, metres
inline D3DMATRIX panel_local = {};// the same in tracking space at open (for the logs)
inline const char* panel_placed = "";
inline bool shown_valid = false;  // shown_model is in the DIB and was offered
inline Model shown_model;
inline float shown_size_m = 0.0f;
inline unsigned uploads = 0;
inline unsigned last_offer_frame = 0;
inline unsigned opens = 0;

// A pose (game units) with its translation in metres.
inline D3DMATRIX InMetres(D3DMATRIX m) {
    const float ws = vrmod::config.world_scale > 0.0f ? vrmod::config.world_scale : 1.0f;
    m._41 /= ws;
    m._42 /= ws;
    m._43 /= ws;
    return m;
}

// A tracking-space pose at (px, py, pz) facing the head point (hx, hy,
// hz): +z toward the head, x level. Metres.
inline bool FacingPose(float px, float py, float pz, float hx, float hy, float hz,
                       D3DMATRIX& out) {
    float zx = hx - px, zy = hy - py, zz = hz - pz;
    const float zn = sqrtf(zx * zx + zy * zy + zz * zz);
    if (zn < 1e-3f)
        return false;
    zx /= zn; zy /= zn; zz /= zn;
    // x = up x z (level), y = z x x.
    float xx = zz, xz = -zx;
    const float xn = sqrtf(xx * xx + xz * xz);
    if (xn < 1e-4f)
        return false;
    xx /= xn; xz /= xn;
    const float xy = 0.0f;
    const float yx = zy * xz - zz * xy;
    const float yy = zz * xx - zx * xz;
    const float yz = zx * xy - zy * xx;
    out = vrmod::Identity();
    out._11 = xx; out._12 = xy; out._13 = xz;
    out._21 = yx; out._22 = yy; out._23 = yz;
    out._31 = zx; out._32 = zy; out._33 = zz;
    out._41 = px; out._42 = py; out._43 = pz;
    return true;
}

// The ring's place at the right hand now: `local` in tracking space, `rel`
// the same relative to the right grip (a point in grip space times the
// grip pose lands in tracking space, so rel = local x grip^-1). False
// without a head or right-hand pose.
inline bool RingPoseAtHand(D3DMATRIX& rel, D3DMATRIX& local) {
    D3DMATRIX head, grip;
    if (!vrmod::Get()->GetHeadPose(head) || !vrmod::Get()->GetHandPose(RING_HAND, grip))
        return false;
    head = InMetres(head);
    grip = InMetres(grip);
    const wheelconfig::Config& c = wheelconfig::config;
    float px = grip._41, py = grip._42 + c.ring_raise_m, pz = grip._43;
    const float tx = head._41 - px, ty = head._42 - py, tz = head._43 - pz;
    const float tn = sqrtf(tx * tx + ty * ty + tz * tz);
    if (tn > fabsf(c.ring_toward_head_m) + RING_MIN_HEAD_GAP_M) {
        px += tx / tn * c.ring_toward_head_m;
        py += ty / tn * c.ring_toward_head_m;
        pz += tz / tn * c.ring_toward_head_m;
    }
    if (!FacingPose(px, py, pz, head._41, head._42, head._43, local))
        return false;
    rel = vrmod::Multiply(local, vrmod::RigidInverse(grip));
    return true;
}

// The fixed place without a right-hand pose: ahead of the head, below eye
// level, in tracking space.
inline bool RingPoseFallback(D3DMATRIX& local) {
    D3DMATRIX head;
    if (!vrmod::Get()->GetHeadPose(head))
        return false;
    head = InMetres(head);
    float fx = -head._31, fz = -head._33;
    const float fn = sqrtf(fx * fx + fz * fz);
    if (fn > 0.2f) {
        fx /= fn;
        fz /= fn;
    } else {
        fx = 0.0f;
        fz = -1.0f;
    }
    return FacingPose(head._41 + fx * RING_FALLBACK_AHEAD_M, head._42 - RING_FALLBACK_DROP_M,
                      head._43 + fz * RING_FALLBACK_AHEAD_M, head._41, head._42, head._43,
                      local);
}

inline void ClosePanel(const char* why) {
    if (!panel_open)
        return;
    vrmod::Get()->HidePanel(PANEL_SLOT);
    panel_open = false;
    shown_valid = false;
    diag::Log("wheel: ring closed (%s; %u upload(s))", why, uploads);
}


inline void UpdatePanel() {
    const wheelui::State& s = wheelui::state;
    bool open = s.open;
    unsigned serial = s.open_serial;
    View v;
    if (open)
        v = ViewOf(s);
    if (!open || !vrmod::Get()->SupportsHudLayer()) {
        ClosePanel(open ? "no panel on this backend" : "the wheel closed");
        return;
    }
    if (!panel_open || serial != shown_serial) {
        D3DMATRIX rel, local;
        int space;
        const char* placed;
        if (RingPoseAtHand(rel, local)) {
            space = RING_HAND;
            placed = "at the right hand, riding it";
        } else if (RingPoseFallback(local)) {
            rel = local;
            space = -1;
            placed = "ahead of the head (no right-hand pose)";
        } else {
            return;  // no head pose yet: try again next pass
        }
        // The space first: the pose below is given in it.
        vrmod::Get()->SetPanelSpace(PANEL_SLOT, space);
        panel_open = true;
        shown_serial = serial;
        panel_space = space;
        panel_pose = rel;
        panel_local = local;
        panel_placed = placed;
        shown_valid = false;
        opens++;
        diag::Log("wheel: ring open%s (%s; %.2f m wide at %.2f, %.2f, %.2f m, raised %.2f m, "
                  "%.2f m toward the head; page %d of %d)",
                  serial == FORCED_SERIAL ? " (forced)" : "", placed,
                  wheelconfig::config.size_m, local._41, local._42, local._43,
                  wheelconfig::config.ring_raise_m, wheelconfig::config.ring_toward_head_m,
                  v.page_count > 0 ? v.page + 1 : 0, v.page_count);
    }
    const Model m = BuildModel(v, ReadableEntity());
    const float size = wheelconfig::config.size_m;
    const bool changed = !shown_valid || !(m == shown_model);
    const bool resized = shown_valid && size != shown_size_m;
    const bool lost = shown_valid && !vrmod::Get()->PanelVisible(PANEL_SLOT) &&
                      wheel::frame_serial - last_offer_frame >= RETRY_FRAMES;
    if (!changed && !resized && !lost)
        return;
    if (changed && !Rasterize(m))
        return;
    vrmod::Get()->ShowPanel(PANEL_SLOT, static_cast<const unsigned int*>(dib_bits), PANEL_PX,
                            PANEL_PX, panel_pose, size);
    uploads++;
    last_offer_frame = wheel::frame_serial;
    shown_model = m;
    shown_valid = true;
    shown_size_m = size;
}

// ---- the hand icon ------------------------------------------------------------

// The icon sits this far out along the hand frame's -X from its origin
// (the centre of the fist), before the hand mesh's scale. The hand frame
// is the controller's grip frame rolled half a turn about Y by the grip
// tuple, so in it -X faces out of the palm (+X is the back of the hand),
// +Y runs toward the wrist and +Z toward the thumb. The distance is the
// hand's half-thickness, shared by every class; kIconClassTrim and the
// [wheel] hand_icon_* knobs trim from there.
constexpr float ICON_PALM_M = 0.04f;

// The icon's per-class trim, in metres along the hand frame's axes (x out
// of the palm, y toward the wrist, z toward the thumb - the senses of the
// hand_icon_offset_* knobs), added on top of the knobs. The hand meshes
// differ per class (each is one rigid piece on one bone, nothing finer to
// anchor to), so each row holds that class's measured residual from the
// shared base. Indexed by the class byte (entity +0x961), in the order of
// wheelcore::kClasses.
struct IconTrim {
    float x, y, z;
};
constexpr IconTrim kIconClassTrim[wheelcore::kClassCount] = {
    {0.0f, 0.0f, 0.0f},   // HUmar
    {0.0f, 0.0f, 0.0f},   // HUnewearl
    {0.0f, 0.0f, 0.0f},   // HUcast
    {0.0f, 0.06f, 0.0f},  // RAmar
    {0.0f, 0.0f, 0.0f},   // RAcast
    {0.0f, 0.0f, 0.0f},   // RAcaseal
    {0.0f, 0.0f, 0.0f},   // FOmarl
    {0.0f, 0.02f, 0.0f},  // FOnewm
    {0.0f, 0.0f, 0.0f},   // FOnewearl
    {0.0f, 0.0f, 0.0f},   // HUcaseal
    {0.0f, 0.0f, 0.0f},   // FOmar
    {0.0f, 0.0f, 0.0f},   // RAmarl
};

// A class byte's trim; zeros for no class or an unknown one.
inline IconTrim IconTrimOf(int cls) {
    if (cls < 0 || cls >= wheelcore::kClassCount)
        return IconTrim{0.0f, 0.0f, 0.0f};
    return kIconClassTrim[cls];
}

// A class byte's key for the log ("none" without one).
inline const char* IconClassKey(int cls) {
    return cls >= 0 && cls < wheelcore::kClassCount ? wheelcore::kClasses[cls].key : "none";
}
// A further push out along the icon's own normal, whatever the trims, so
// it never shares depth with the hand's surface (no depth fighting).
constexpr float ICON_PUSH_M = 0.002f;
constexpr int FLASH_FRAMES = 8;      // presented frames a fire's flash lasts
constexpr float FLASH_GROW = 0.25f;  // the icon's extra size at the flash's start

inline bool hand_show = false;  // this pass: an entry to show and the wheel on
inline Entry hand_entry;
inline Status hand_status;
inline unsigned last_fire_serial = 0;
inline bool fire_serial_seen = false;
inline unsigned flash_until = 0;  // wheel::frame_serial
inline unsigned flashes = 0;
inline bool drawn_this_pass = false;
inline bool drawn_last_pass = false;
inline bool world_seen_this_pass = false;
inline unsigned icon_draws = 0;
inline bool icon_first_logged = false;
inline D3DMATRIX last_icon_world = {};
inline bool last_icon_world_valid = false;
inline hands::HandSource last_icon_source = hands::HandSource::BareGrip;

// 0..1: how far into a fire's flash this frame is (1 = its start).
inline float FlashLevel() {
    if (flash_until <= wheel::frame_serial)
        return 0.0f;
    const float f = (float)(flash_until - wheel::frame_serial) / (float)FLASH_FRAMES;
    return f > 1.0f ? 1.0f : f;
}

// The icon's frame in the left hand's drawn frame (game units; see
// ICON_PALM_M for its axes). Rows: the icon's x (right) = hand -Z (away
// from the thumb), its y (up) = hand -Y (away from the wrist), its z (out,
// toward the viewer) = hand -X (out of the palm); centred ICON_PALM_M out
// along -X. That stands it upright on the palm as the player looks at it.
// First hand_icon_spin_deg turns it in its own plane about its normal
// (positive = counter-clockwise as seen from the front).
// hand_icon_tilt_deg then turns it about the hand's Z axis (the axis
// through the fist), around the hand: positive from -X toward -Y, 180
// puts it on the back of the hand. The offsets follow, along the hand's
// axes: the knobs plus the class's row of kIconClassTrim (cls = the class
// byte, -1 for none). Positions are multiplied by mesh_scale (the drawn
// hand's scale); the icon's size is not.
inline D3DMATRIX IconLocal(float mesh_scale, int cls) {
    const wheelconfig::Config& c = wheelconfig::config;
    const IconTrim trim = IconTrimOf(cls);
    const float ws = vrmod::config.world_scale * mesh_scale;
    const float deg = 3.14159265f / 180.0f;
    const float s = c.hand_icon_spin_deg * deg;
    D3DMATRIX spin = vrmod::Identity();  // about the icon's own z
    spin._11 = cosf(s); spin._12 = sinf(s);
    spin._21 = -sinf(s); spin._22 = cosf(s);
    D3DMATRIX base = vrmod::Identity();
    base._11 = 0.0f;  base._12 = 0.0f;  base._13 = -1.0f;
    base._21 = 0.0f;  base._22 = -1.0f; base._23 = 0.0f;
    base._31 = -1.0f; base._32 = 0.0f;  base._33 = 0.0f;
    base._41 = -ICON_PALM_M * ws;
    const float t = c.hand_icon_tilt_deg * deg;
    D3DMATRIX rz = vrmod::Identity();  // about the hand's z
    rz._11 = cosf(t); rz._12 = sinf(t);
    rz._21 = -sinf(t); rz._22 = cosf(t);
    D3DMATRIX off = vrmod::Identity();
    off._41 = (c.hand_icon_offset_x_m + trim.x) * ws;
    off._42 = (c.hand_icon_offset_y_m + trim.y) * ws;
    off._43 = (c.hand_icon_offset_z_m + trim.z) * ws;
    return vrmod::Multiply(vrmod::Multiply(vrmod::Multiply(spin, base), rz), off);
}

// What the icon shows: while the ring is open, what a release would arm -
// the lit segment, nothing for an empty one or with none lit; else the
// armed choice.
inline Entry HandEntry() {
    const wheelui::State& s = wheelui::state;
    if (s.open) {
        if (s.highlight < 0 || s.highlight >= kSegments || s.page_count <= 0)
            return Entry();
        return wheelcore::Clamped(s.segments[s.highlight]);
    }
    return wheel::Armed() ? wheel::Choice() : Entry();
}

inline void UpdateHandIcon() {
    const wheelui::State& s = wheelui::state;
    if (!fire_serial_seen) {
        fire_serial_seen = true;
        last_fire_serial = s.fire_serial;
    }
    if (s.fire_serial != last_fire_serial) {
        last_fire_serial = s.fire_serial;
        flash_until = wheel::frame_serial + FLASH_FRAMES;
        flashes++;
    }
    hand_show = false;
    if (!wheelconfig::config.enabled || !wheelconfig::config.hand_icon ||
        !gamecam::DrivesView())
        return;
    const Entry e = HandEntry();
    if (e.kind == Kind::None)
        return;
    hand_entry = e;
    hand_status = StatusOf(e, ReadableEntity());
    hand_show = true;
}

// Once per BeginScene pass, after controller::OnFrame (and the input
// side's state for this frame).
inline void Update() {
    drawn_last_pass = drawn_this_pass;
    drawn_this_pass = false;
    world_seen_this_pass = false;
    UpdatePanel();
    UpdateHandIcon();
}

struct IconVertex {
    float x, y, z;
    DWORD color;
    float u, v;
};
constexpr DWORD ICON_FVF = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1;

inline void DrawNow(IDirect3DDevice9* dev, const char* trigger);

inline bool DrawGated() {
    return !hand_show || !gamecam::DrivesView() || !stereo::WantsDuplication() ||
           resolution::passthrough;
}

// Told about every draw before it runs, beside hands::MaybeDraw: the icon
// draws at the first 2D draw of a pass that drew the world.
inline void MaybeDraw(IDirect3DDevice9* dev, bool scene_draw) {
    if (DrawGated())
        return;
    if (scene_draw) {
        if (!stereo::GameViewportIsBoxed())
            world_seen_this_pass = true;
        return;
    }
    if (drawn_this_pass || !world_seen_this_pass)
        return;
    if (stereo::GameViewportIsBoxed())
        return;  // a boxed 3D-UI pass: its view is not the scene's
    DrawNow(dev, "first 2D draw");
}

// EndScene: a pass that drew the world but no 2D draw.
inline void OnPassEnd(IDirect3DDevice9* dev) {
    if (DrawGated() || drawn_this_pass || !world_seen_this_pass ||
        stereo::GameViewportIsBoxed())
        return;
    DrawNow(dev, "end of scene");
}

inline const char* TupleName() {
    return weapongrip::current_is_gun ? "gun" : "melee";
}

inline void DrawNow(IDirect3DDevice9* dev, const char* trigger) {
    drawn_this_pass = true;
    D3DMATRIX hand_frame;
    float mesh_scale = 1.0f;
    hands::HandSource source = hands::HandSource::BareGrip;
    if (!hands::DrawnHandFrame(0, hand_frame, mesh_scale, source))
        return;
    const int cls = LocalClass(ReadableEntity());
    if (!EnsureTextures(dev))
        return;
    int tex = 0, cx = 0, cy = 0;
    if (!CellOf(hand_entry, tex, cx, cy))
        return;

    const float flash = FlashLevel();
    const float ws = vrmod::config.world_scale;
    const float hw = 0.5f * wheelconfig::config.hand_icon_size_m * ws * (1.0f + FLASH_GROW * flash);
    const float hh = hw * (float)CELL_H / (float)CELL_W;
    // Half a texel in from the cell's edges: linear filtering would
    // otherwise pull in the neighbouring cells.
    const float u0 = (cx + 0.5f) / TEX_SIZE, u1 = (cx + CELL_W - 0.5f) / TEX_SIZE;
    const float v0 = (cy + 0.5f) / TEX_SIZE, v1 = (cy + CELL_H - 0.5f) / TEX_SIZE;
    // Stage 0 doubles texture x diffuse, so the diffuse at half strength
    // is the plain icon and a flash brightens it toward twice.
    const uint32_t mult = kAvailColour[hand_status.avail];
    const float k = 0.5f + 0.5f * flash;
    const DWORD color = 0xFF000000u | ((DWORD)(((mult >> 16) & 255) * k + 0.5f) << 16) |
                        ((DWORD)(((mult >> 8) & 255) * k + 0.5f) << 8) |
                        (DWORD)((mult & 255) * k + 0.5f);
    const float pz = ICON_PUSH_M * ws;
    const IconVertex q[4] = {
        {-hw, hh, pz, color, u0, v0},
        {hw, hh, pz, color, u1, v0},
        {-hw, -hh, pz, color, u0, v1},
        {hw, -hh, pz, color, u1, v1},
    };
    const D3DMATRIX world = vrmod::Multiply(IconLocal(mesh_scale, cls), hand_frame);

    // Save everything set below. DrawPrimitiveUP clears stream 0.
    static constexpr D3DRENDERSTATETYPE kRs[] = {
        D3DRS_LIGHTING, D3DRS_FOGENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,
        D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_ZENABLE,
        D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
        D3DRS_CLIPPLANEENABLE, D3DRS_COLORWRITEENABLE, D3DRS_CULLMODE};
    static constexpr DWORD kRsValue[] = {
        FALSE, FALSE, TRUE, D3DBLEND_SRCALPHA,
        D3DBLEND_INVSRCALPHA, D3DBLENDOP_ADD, FALSE, D3DZB_TRUE,
        FALSE, D3DCMP_LESSEQUAL, FALSE, FALSE,
        0,
        // Colour only: the eye image's alpha is left as the scene made it.
        D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE,
        D3DCULL_NONE};
    static constexpr D3DTEXTURESTAGESTATETYPE kTs[] = {
        D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP,
        D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2, D3DTSS_TEXCOORDINDEX,
        D3DTSS_TEXTURETRANSFORMFLAGS};
    static constexpr DWORD kTsValue[] = {
        D3DTOP_MODULATE2X, D3DTA_TEXTURE, D3DTA_DIFFUSE, D3DTOP_MODULATE,
        D3DTA_TEXTURE, D3DTA_DIFFUSE, 0,
        D3DTTFF_DISABLE};
    static constexpr D3DSAMPLERSTATETYPE kSs[] = {
        D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU,
        D3DSAMP_ADDRESSV};
    static constexpr DWORD kSsValue[] = {
        D3DTEXF_LINEAR, D3DTEXF_LINEAR, D3DTEXF_NONE, D3DTADDRESS_CLAMP,
        D3DTADDRESS_CLAMP};
    constexpr size_t kNRs = sizeof(kRs) / sizeof(kRs[0]);
    constexpr size_t kNTs = sizeof(kTs) / sizeof(kTs[0]);
    constexpr size_t kNSs = sizeof(kSs) / sizeof(kSs[0]);
    static_assert(kNRs == sizeof(kRsValue) / sizeof(kRsValue[0]), "render states");
    static_assert(kNTs == sizeof(kTsValue) / sizeof(kTsValue[0]), "stage states");
    static_assert(kNSs == sizeof(kSsValue) / sizeof(kSsValue[0]), "sampler states");

    DWORD rs_saved[kNRs], ts_saved[kNTs], ss_saved[kNSs];
    for (size_t i = 0; i < kNRs; i++)
        dev->GetRenderState(kRs[i], &rs_saved[i]);
    for (size_t i = 0; i < kNTs; i++)
        dev->GetTextureStageState(0, kTs[i], &ts_saved[i]);
    for (size_t i = 0; i < kNSs; i++)
        dev->GetSamplerState(0, kSs[i], &ss_saved[i]);
    DWORD saved_stage1_color = 0, saved_stage1_alpha = 0;
    dev->GetTextureStageState(1, D3DTSS_COLOROP, &saved_stage1_color);
    dev->GetTextureStageState(1, D3DTSS_ALPHAOP, &saved_stage1_alpha);
    D3DMATRIX saved_world, saved_view, saved_proj;
    dev->GetTransform(D3DTS_WORLD, &saved_world);
    dev->GetTransform(D3DTS_VIEW, &saved_view);
    dev->GetTransform(D3DTS_PROJECTION, &saved_proj);
    D3DVIEWPORT9 saved_vp = {};
    dev->GetViewport(&saved_vp);
    DWORD saved_fvf = 0;
    dev->GetFVF(&saved_fvf);
    IDirect3DVertexDeclaration9* saved_decl = nullptr;
    dev->GetVertexDeclaration(&saved_decl);
    IDirect3DVertexShader9* saved_vs = nullptr;
    IDirect3DPixelShader9* saved_ps = nullptr;
    dev->GetVertexShader(&saved_vs);
    dev->GetPixelShader(&saved_ps);
    IDirect3DBaseTexture9* saved_tex = nullptr;
    dev->GetTexture(0, &saved_tex);
    IDirect3DVertexBuffer9* saved_vb = nullptr;
    UINT saved_vb_offset = 0, saved_vb_stride = 0;
    dev->GetStreamSource(0, &saved_vb, &saved_vb_offset, &saved_vb_stride);

    for (size_t i = 0; i < kNRs; i++)
        dev->SetRenderState(kRs[i], kRsValue[i]);
    // [wheel] hand_icon_depth_test 0: over the hand and the weapon, never
    // hidden.
    if (!wheelconfig::config.hand_icon_depth_test)
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    for (size_t i = 0; i < kNTs; i++)
        dev->SetTextureStageState(0, kTs[i], kTsValue[i]);
    for (size_t i = 0; i < kNSs; i++)
        dev->SetSamplerState(0, kSs[i], kSsValue[i]);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(ICON_FVF);
    dev->SetTexture(0, art_tex[tex]);
    dev->SetTransform(D3DTS_WORLD, &world);
    stereo::Duplicate(dev, [&](int) {
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(IconVertex));
    });

    // Restore. The eyes' viewport and matrices go back too.
    for (size_t i = 0; i < kNRs; i++)
        dev->SetRenderState(kRs[i], rs_saved[i]);
    for (size_t i = 0; i < kNTs; i++)
        dev->SetTextureStageState(0, kTs[i], ts_saved[i]);
    for (size_t i = 0; i < kNSs; i++)
        dev->SetSamplerState(0, kSs[i], ss_saved[i]);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, saved_stage1_color);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, saved_stage1_alpha);
    dev->SetTransform(D3DTS_WORLD, &saved_world);
    dev->SetTransform(D3DTS_VIEW, &saved_view);
    dev->SetTransform(D3DTS_PROJECTION, &saved_proj);
    dev->SetViewport(&saved_vp);
    dev->SetVertexShader(saved_vs);
    dev->SetPixelShader(saved_ps);
    if (saved_vs != nullptr)
        saved_vs->Release();
    if (saved_ps != nullptr)
        saved_ps->Release();
    if (saved_decl != nullptr) {
        dev->SetVertexDeclaration(saved_decl);
        saved_decl->Release();
    }
    if (saved_fvf != 0)
        dev->SetFVF(saved_fvf);
    dev->SetTexture(0, saved_tex);
    if (saved_tex != nullptr)
        saved_tex->Release();
    if (saved_vb != nullptr) {
        dev->SetStreamSource(0, saved_vb, saved_vb_offset, saved_vb_stride);
        saved_vb->Release();
    }

    icon_draws++;
    last_icon_world = world;
    last_icon_world_valid = true;
    last_icon_source = source;
    if (!icon_first_logged) {
        icon_first_logged = true;
        diag::Log("wheel: hand icon first drawn (%s, %s; at %s; %.3f m wide, offset %.3f, "
                  "%.3f, %.3f m, spin %.0f deg, tilt %.0f deg, depth test %s; frame %s, %s "
                  "tuple, mesh scale %.2f; class %s trim %.3f, %.3f, %.3f m)",
                  wheelcore::KeyOf(hand_entry), kAvailName[hand_status.avail], trigger,
                  wheelconfig::config.hand_icon_size_m, wheelconfig::config.hand_icon_offset_x_m,
                  wheelconfig::config.hand_icon_offset_y_m,
                  wheelconfig::config.hand_icon_offset_z_m, wheelconfig::config.hand_icon_spin_deg,
                  wheelconfig::config.hand_icon_tilt_deg,
                  wheelconfig::config.hand_icon_depth_test ? "on" : "off",
                  hands::HandSourceName(source), TupleName(), mesh_scale, IconClassKey(cls),
                  IconTrimOf(cls).x, IconTrimOf(cls).y, IconTrimOf(cls).z);
    }
}


}  // namespace wheeldraw
