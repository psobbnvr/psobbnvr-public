#pragma once

// The spell wheel's input side: the hold, the eight-way pick on the right
// stick, paging on the triggers, the release that arms the choice, the
// cancel rules and the bare-swing fire request. Fills wheelui::state
// (psobbvr_wheel_ui.hpp) for the drawing side.
//
// Holding the wheel action (a keymap action with no keys, default X) for
// [wheel] open_delay_ms opens the ring; a shorter tap does nothing. While
// it is open:
//  - the lit segment follows the live right stick, 0 = up, clockwise. A
//    segment lights at half deflection and holds until the stick is
//    PICK_HYSTERESIS_DEG past the boundary it shares with its neighbour.
//    The highlight goes out when the stick comes back to the centre
//    (below PICK_OUT_RADIUS); between the two radii it holds as it is, so
//    it never flickers at the edge. Nothing is lit until the stick moves;
//  - a fresh pull of the right trigger turns to the next page, the left
//    trigger to the previous one, wrapping. A trigger already held when
//    the ring opens turns nothing. The controller keeps both triggers
//    away from the palette meanwhile;
//  - the right stick belongs to the wheel: the controller takes no turn,
//    menu arrow or hotkey flick from it, and keeps it so after the close
//    until the stick re-centres.
// Releasing the button chooses what the stick points at then: the lit
// segment's entry is armed (wheel::Arm); an empty segment, or nothing lit
// (the stick centred), clears the choice (wheel::Clear). A tap released
// before the ring opens changes nothing. A menu taking focus, the camera
// takeover stopping, the on-screen keyboard opening, the controllers going
// away, [wheel] enabled turning off, a settings reload or a change of
// character close the ring without choosing; the button must then be let
// go before the ring can open again.
//
// controller::OnFrame runs twice per presented frame on the same
// controller state, so everything here acts on changes, never per call.

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "psobbvr_bindings.hpp"
#include "psobbvr_log.hpp"
#include "psobbvr_vr.hpp"
#include "psobbvr_wheel.hpp"
#include "psobbvr_wheel_char.hpp"
#include "psobbvr_wheel_config.hpp"
#include "psobbvr_wheel_core.hpp"
#include "psobbvr_wheel_ui.hpp"

namespace wheelinput {

using wheelcore::Entry;
using wheelcore::Kind;

constexpr int HAND_LEFT = 0, HAND_RIGHT = 1;
// Stick deflection (radial, 0..1) that lights a segment...
constexpr float PICK_RADIUS = 0.5f;
// ...and below which the highlight goes out: the controller's own "back at
// the centre" (the stick stays the wheel's until it gets there too).
constexpr float PICK_OUT_RADIUS = bindings::STICK_CENTER;
constexpr float SECTOR_DEG = 360.0f / wheelcore::kSegments;
// How far past a shared boundary the stick must go before the neighbour
// lights.
constexpr float PICK_HYSTERESIS_DEG = 8.0f;
// Haptics: the on-screen keyboard's hover tick for a segment or page
// change, its key press for the choice.
constexpr float TICK_S = 0.012f, TICK_AMP = 0.35f;
constexpr float CHOOSE_S = 0.03f, CHOOSE_AMP = 0.7f;

inline bool owns_stick = false;      // the right stick is the wheel's
inline bool button_held = false;     // the wheel action was held at the last Tick
inline bool holding = false;         // held and counting toward the open
inline bool need_release = false;    // a cancel, or a hold that could not open:
                                     // nothing opens until the button is let go
inline LONGLONG hold_start_qpc = 0;
inline LONGLONG qpf = 0;
inline unsigned open_generation = 0; // wheelconfig::generation at the hold's start
inline unsigned seen_character = 0;  // wheel::character_serial last acted on
inline bool prev_trig[2] = {};       // the paging edges' last trigger state
inline char last_close[128] = "none yet";
inline unsigned fires_posted = 0;

// This frame's bare-swing status, written by the controller.
struct SwingStatus {
    bool eligible = false;               // a castable choice, the wheel on, field play
    bool armed = false;                  // the detector is armed for it
    const char* blocked = "not evaluated yet";  // why not, when not armed
};
inline SwingStatus swing;

inline int WheelAction() {
    static const int act = bindcore::FindAction("wheel");
    return act;
}

inline bool Open() { return wheelui::state.open; }
inline bool OwnsStick() { return owns_stick; }
// The wheel's button is down (the ring is open or about to be): a swing
// then fires nothing of the wheel's.
inline bool ButtonHeld() { return button_held; }

inline const char* ClassKey(int cls) {
    return cls >= 0 && cls < wheelcore::kClassCount ? wheelcore::kClasses[cls].key : "unknown";
}

inline LONGLONG Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (qpf == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpf = f.QuadPart;
    }
    return t.QuadPart;
}

inline void Pulse(int hand, float seconds, float amplitude) {
    if (wheelconfig::config.haptics)
        vrmod::Get()->HapticPulse(hand, seconds, amplitude);
}

// The local player's class byte, -1 without a player.
inline int LocalClass() {
    const uintptr_t e = wheel::LocalEntity();
    if (e == 0 || !diag::Accessible(e + wheel::ENT_CLASS_OFF, 1, false))
        return -1;
    const int cls = *reinterpret_cast<const uint8_t*>(e + wheel::ENT_CLASS_OFF);
    return cls < wheelcore::kClassCount ? cls : -1;
}

// The shown page's entries into the view state.
inline void FillSegments() {
    wheelui::State& s = wheelui::state;
    for (int i = 0; i < wheelcore::kSegments; i++)
        s.segments[i] = Entry();
    if (s.cls < 0 || s.page < 0 || s.page >= s.page_count)
        return;
    const wheelcore::Layout* lay = wheelchar::LayoutFor(s.cls);
    if (lay == nullptr || s.page >= lay->pages)
        return;
    const wheelcore::Page& p = lay->page[s.page];
    for (int i = 0; i < wheelcore::kSegments; i++)
        s.segments[i] = wheelcore::Clamped(p.seg[i]);
}

// The stick's direction in degrees, 0 = up, clockwise, 0..360.
inline float StickAngle(float x, float y) {
    float deg = atan2f(x, y) * (180.0f / 3.14159265f);
    if (deg < 0.0f)
        deg += 360.0f;
    return deg;
}

inline int SectorAt(float deg) {
    return (int)floorf((deg + SECTOR_DEG * 0.5f) / SECTOR_DEG) % wheelcore::kSegments;
}

// Is the stick still inside `sector`, widened by the hysteresis?
inline bool StillIn(int sector, float deg) {
    float d = deg - sector * SECTOR_DEG;
    while (d > 180.0f)
        d -= 360.0f;
    while (d <= -180.0f)
        d += 360.0f;
    return fabsf(d) <= SECTOR_DEG * 0.5f + PICK_HYSTERESIS_DEG;
}

// The lit segment from the live stick. Going out ticks nothing: it happens
// whenever the thumb lets go, and the release has its own pulse.
inline void Pick(const vrmod::ControllerState& cs) {
    wheelui::State& s = wheelui::state;
    const float x = cs.stick_x[HAND_RIGHT], y = cs.stick_y[HAND_RIGHT];
    const float r = sqrtf(x * x + y * y);
    if (r < PICK_OUT_RADIUS) {
        if (s.highlight >= 0) {
            s.highlight = -1;
            s.pick_serial++;
        }
        return;
    }
    if (r < PICK_RADIUS)
        return;  // between the radii: as it is
    const float deg = StickAngle(x, y);
    int sector = s.highlight;
    if (sector < 0 || !StillIn(sector, deg))
        sector = SectorAt(deg);
    if (sector == s.highlight)
        return;
    s.highlight = sector;
    s.pick_serial++;
    Pulse(HAND_RIGHT, TICK_S, TICK_AMP);
}

inline void Page(const vrmod::ControllerState& cs) {
    wheelui::State& s = wheelui::state;
    const float press = vrmod::config.controller_press;
    bool fresh[2];
    for (int h = 0; h < 2; h++) {
        const bool now = cs.trigger[h] > press;
        fresh[h] = now && !prev_trig[h];
        prev_trig[h] = now;
    }
    // Both pulled at once cancel out.
    const int step = (fresh[HAND_RIGHT] ? 1 : 0) - (fresh[HAND_LEFT] ? 1 : 0);
    if (step == 0 || s.page_count <= 1)
        return;
    s.page = (s.page + step + s.page_count) % s.page_count;
    wheelchar::SetLastPage(s.page);
    FillSegments();  // the highlight stays: the thumb still points there
    s.pick_serial++;
    const int hand = step > 0 ? HAND_RIGHT : HAND_LEFT;
    Pulse(hand, TICK_S, TICK_AMP);
    diag::Log("wheel: page %d / %d (%s trigger)", s.page + 1, s.page_count,
              hand == HAND_RIGHT ? "right" : "left");
}

inline void OpenRing(const vrmod::ControllerState& cs) {
    wheelui::State& s = wheelui::state;
    s.cls = LocalClass();
    const char* why = "no character";
    const wheelcore::Layout* lay = s.cls >= 0 ? wheelchar::LayoutFor(s.cls, &why) : nullptr;
    s.page_count = lay != nullptr ? wheelcore::ClampInt(lay->pages, 0, wheelcore::kMaxPages) : 0;
    s.page = s.page_count > 0 ? wheelcore::ClampInt(wheelchar::LastPage(), 0, s.page_count - 1) : 0;
    s.highlight = -1;
    FillSegments();
    s.pick_serial++;
    // A trigger already down (a charge running, say) must not turn a page.
    const float press = vrmod::config.controller_press;
    for (int h = 0; h < 2; h++)
        prev_trig[h] = cs.trigger[h] > press;
    s.open = true;
    s.open_serial++;
    const wheelchar::Who who = wheelchar::Current();
    diag::Log("wheel: open (guild card %u slot %d, class %s, %d page(s), page %d)%s%s",
              who.guild_card, who.slot + 1, ClassKey(s.cls), s.page_count,
              s.page_count > 0 ? s.page + 1 : 0, s.page_count == 0 ? " - " : "",
              s.page_count == 0 ? why : "");
}

// Close without choosing.
inline void CloseUnchosen(const char* why) {
    wheelui::state.open = false;
    snprintf(last_close, sizeof(last_close), "cancelled: %s", why);
    diag::Log("wheel: closed without choosing (%s)", why);
}

// The button let go with the ring open: choose what the stick points at
// now. Nothing lit (the stick centred) clears the choice.
inline void Release() {
    wheelui::State& s = wheelui::state;
    s.open = false;
    const int h = s.highlight;
    if (h < 0 || h >= wheelcore::kSegments) {
        snprintf(last_close, sizeof(last_close), "released with nothing lit: choice cleared (was %s)",
                 wheel::Armed() ? wheelcore::KeyOf(wheel::Choice()) : "nothing armed");
        diag::Log("wheel: released with nothing lit - choice cleared");
        wheel::Clear();
        Pulse(HAND_LEFT, CHOOSE_S, CHOOSE_AMP);
        return;
    }
    const Entry e = s.segments[h];
    const int page = s.page_count > 0 ? s.page + 1 : 0;
    if (e.kind == Kind::None) {
        snprintf(last_close, sizeof(last_close), "chose %s on page %d: empty, choice cleared",
                 wheelcore::kCompass[h], page);
        diag::Log("wheel: chose %s on page %d - an empty segment, the choice is cleared",
                  wheelcore::kCompass[h], page);
        wheel::Clear();
    } else {
        snprintf(last_close, sizeof(last_close), "chose %s on page %d: %s", wheelcore::kCompass[h],
                 page, wheelcore::KeyOf(e));
        diag::Log("wheel: chose %s on page %d - %s", wheelcore::kCompass[h], page,
                  wheelcore::KeyOf(e));
        wheel::Arm(e);
    }
    Pulse(HAND_LEFT, CHOOSE_S, CHOOSE_AMP);
}

// Why the ring cannot be open now, or nullptr.
inline const char* Blocked(bool menu_mode, bool gameplay_drives) {
    if (!gameplay_drives)
        return "the camera takeover stopped";
    if (menu_mode)
        return "a menu took focus";
    if (!wheelconfig::config.enabled)
        return "[wheel] enabled is 0";
    if (!vrmod::Get()->SupportsHudLayer())
        return "no panel layer on this VR backend";
    return nullptr;
}

// From controller::OnFrame, right after bindings::Evaluate (ActionHeld is
// this frame's) and before the triggers and sticks read the frame.
inline void Tick(const vrmod::ControllerState& cs, bool menu_mode, bool gameplay_drives) {
    wheelui::State& s = wheelui::state;
    const int act = WheelAction();
    const bool held = act >= 0 && bindings::ActionHeld(act);
    button_held = held;
    wheelchar::Update();
    // A change of character (wheel::WatchCharacter, which cleared the
    // choice) ends any ring or hold of the old one's.
    if (wheel::character_serial != seen_character) {
        seen_character = wheel::character_serial;
        if (s.open)
            CloseUnchosen("the character changed");
        holding = false;
        need_release = true;
    }
    const char* blocked = Blocked(menu_mode, gameplay_drives);
    if (s.open) {
        // A reload resets the bindings, which reads as a release: the
        // generation is tested first so it cancels instead.
        if (wheelconfig::generation != open_generation) {
            CloseUnchosen("the settings reloaded");
            need_release = true;
        } else if (blocked != nullptr) {
            CloseUnchosen(blocked);
            need_release = true;
        } else if (!held) {
            Release();
        } else {
            Pick(cs);
            Page(cs);
        }
    } else if (!held) {
        holding = false;
        need_release = false;
    } else if (!need_release) {
        const LONGLONG now = Now();
        if (!holding) {
            if (blocked != nullptr) {
                need_release = true;  // pressed where it cannot open
            } else {
                holding = true;
                hold_start_qpc = now;
                open_generation = wheelconfig::generation;
            }
        } else if (blocked != nullptr || wheelconfig::generation != open_generation) {
            holding = false;
            need_release = true;
        }
        if (holding && qpf > 0 &&
            double(now - hold_start_qpc) * 1000.0 / double(qpf) >=
                (double)wheelconfig::config.open_delay_ms) {
            holding = false;
            OpenRing(cs);
        }
    }
    // The stick stays the wheel's after the close until it re-centres, so
    // a push still held when the button lets go neither turns the player
    // nor moves a menu cursor.
    const float x = cs.stick_x[HAND_RIGHT], y = cs.stick_y[HAND_RIGHT];
    if (s.open)
        owns_stick = true;
    else if (owns_stick && sqrtf(x * x + y * y) < bindings::STICK_CENTER)
        owns_stick = false;
}

// The controller's neutral path (no controller input, or the on-screen
// keyboard took the controllers): close without choosing, and wait for
// the button's release before opening again.
inline void Cancel(const char* why) {
    holding = false;
    need_release = true;
    button_held = false;
    swing.eligible = swing.armed = false;
    swing.blocked = why;
    if (wheelui::state.open)
        CloseUnchosen(why);
}

// A bare swing fired the wheel's arming (psobbvr_controller.hpp): post the
// fire request, which the game side serves on the player's next idle,
// walk or run tick, and flash the hand icon when it was posted.
inline void PostSwingFire(int hand) {
    diag::Log("wheel: bare swing (%s hand) - posting a fire request",
              hand == HAND_LEFT ? "left" : "right");
    wheel::RequestFire();
    if (wheel::fire_pending) {
        fires_posted++;
        wheelui::state.fire_serial++;
    }
}


}  // namespace wheelinput
