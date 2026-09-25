#pragma once

// The in-game side of the binding table: loads psobbvr.ini [bindings] over
// the defaults (psobbvr_bindings_core.hpp), reloads when the file changes
// (checked about once a second), and turns each frame's buttons into keys
// for psobbvr_controller.hpp.
//
// The chord is decided at the button's press edge: a grip already down
// makes it a chord for the whole hold; a grip arriving later changes
// nothing. Hold actions keep their keys down until release; one-shot
// actions (Tab, hotkeys) press for kOneShotFrames even if released sooner
// (the game needs a full tick). Right-stick flicks are derived buttons -
// a firm push in one direction presses, near center releases - in the
// field only, and only with a modifier held (StickIsSelector suppresses
// the turn then).

#include <share.h>
#include <windows.h>

#include "psobbvr_bindings_core.hpp"
#include "psobbvr_log.hpp"

// d3d8's own export (d3d8to9.cpp) - the same recenter the panel button
// calls.
extern "C" void WINAPI PsobbvrVrRecenter(void);

namespace bindings {

inline bindcore::Table table;
inline bool initialized = false;
inline char ini_path[MAX_PATH] = "";
inline FILETIME ini_write_time = {};
inline int poll_countdown = 0;
constexpr int POLL_EVERY_CALLS = 60;  // two mapper calls per 30 Hz tick ~ 1 s

// Per-button runtime state.
inline bool prev[bindcore::BTN_COUNT] = {};
inline int latched[bindcore::BTN_COUNT] = {-1, -1, -1, -1, -1, -1, -1,
                                            -1, -1, -1, -1, -1, -1, -1};  // table index held since press
inline int shot_frames[bindcore::BTN_COUNT] = {};
inline int shot_binding[bindcore::BTN_COUNT] = {};
inline int flick_dir = -1;                     // latched BTN_RSTICK_* or -1
inline bool ctrl_held = false;                 // a hold binding emitted Ctrl this frame
// Hotkey press hook (psobbvr_controller.hpp): called at a hotkey chord's
// press edge; returning true consumes the press (armed for the swing).
inline bool (*hotkey_press)(int action, unsigned char key) = nullptr;
// Quick-menu latch. The Quick Menu cycles tabs with Right Arrow, and the
// game's UI focus cannot tell it from other menus, so we latch it
// ourselves: the chord that opened it (quick_menu, or quick_chat with the
// bank-2 Ctrl held = Ctrl+End) sets the latch; pressed again it sends a
// bare Right (next tab), and the right stick keeps turning. Cleared when
// focus returns to the field, after a short grace.
inline bool quick_menu_open = false;
inline int quick_menu_grace = 0;               // mapper calls left before the field flag may clear the latch
inline int quick_cycle_frames = 0;             // a next-tab Right press in flight
constexpr int kQuickMenuGraceCalls = 30;       // ~0.5 s at two mapper calls per 30 Hz tick
constexpr float STICK_FIRM = 0.6f;             // flick press threshold
constexpr float STICK_CENTER = 0.35f;          // flick release threshold

inline void Reset() {
    for (int i = 0; i < bindcore::BTN_COUNT; i++) {
        prev[i] = false;
        latched[i] = -1;
        shot_frames[i] = 0;
        shot_binding[i] = -1;
    }
    flick_dir = -1;
    ctrl_held = false;
}

inline void LogLine(void*, const char* line) { diag::Log("bindings: %s", line); }

inline bool ReadWriteTime(FILETIME& ft) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(ini_path, GetFileExInfoStandard, &fad))
        return false;
    ft = fad.ftLastWriteTime;
    return true;
}

// Dump the active table to the log.
inline void Dump() {
    diag::Log("bindings: %d chords active (%s; %d ini lines accepted, %d rejected)",
              table.count,
              table.section_present ? "[bindings] section + built-in defaults"
                                    : "no [bindings] section - built-in defaults",
              table.accepted_lines, table.rejected_lines);
    for (int a = 0; a < bindcore::kActionCount; a++) {
        char chords[256];
        bindcore::ActionChordsText(table, a, chords, sizeof(chords));
        diag::Log("bindings:   %-11s = %-40s %s", bindcore::kActions[a].name, chords,
                  bindcore::kActions[a].label);
    }
}

// (Re)load the section. A present but unreadable ini (mid-rewrite, locked)
// keeps the previous table; a missing ini or section means the defaults.
inline void Load(const char* why) {
    if (ini_path[0] == '\0') {
        if (!GetCurrentDirectoryA(MAX_PATH, ini_path))
            ini_path[0] = '\0';
        else
            strcat_s(ini_path, "\\psobbvr.ini");
    }
    FILETIME ft = {};
    const bool exists = ReadWriteTime(ft);
    if (exists)
        ini_write_time = ft;
    if (exists) {
        FILE* f = _fsopen(ini_path, "r", _SH_DENYNO);
        if (f == nullptr) {
            if (initialized) {
                diag::Log("bindings: %s - ini unreadable right now, keeping "
                          "the previous table", why);
                return;
            }
        } else {
            fclose(f);
        }
    }
    // GetPrivateProfileSection returns "k=v\0k=v\0\0" (0 = no section).
    static char buf[16384];
    const DWORD n = GetPrivateProfileSectionA("bindings", buf, sizeof(buf), ini_path);
    const char* lines[128];
    int nlines = 0;
    for (const char* p = buf; n > 0 && *p != '\0' && nlines < 128;
         p += strlen(p) + 1)
        lines[nlines++] = p;
    diag::Log("bindings: loading (%s) from %s: %d section lines", why, ini_path,
              nlines);
    bindcore::Table t;
    bindcore::Build(lines, nlines, t, LogLine, nullptr);
    table = t;
    Reset();
    initialized = true;
    Dump();
}

// Once per mapper call: lazy first load, then the write-time poll.
inline void Poll() {
    if (!initialized) {
        Load("startup");
        return;
    }
    if (++poll_countdown < POLL_EVERY_CALLS)
        return;
    poll_countdown = 0;
    FILETIME ft = {};
    if (!ReadWriteTime(ft))
        return;
    if (CompareFileTime(&ft, &ini_write_time) != 0) {
        ini_write_time = ft;
        Load("ini changed");
    }
}

// Derive the flick buttons from the right stick (raw axes). Field only:
// in menu mode the stick is Up/Down/Left/Right navigation.
inline void StickFlick(float x, float y, bool field, bool now[bindcore::BTN_COUNT]) {
    const float ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    const float mag = ax > ay ? ax : ay;
    if (!field) {
        flick_dir = -1;
    } else if (flick_dir < 0) {
        if (mag >= STICK_FIRM)
            flick_dir = ay >= ax ? (y > 0 ? bindcore::BTN_RSTICK_UP : bindcore::BTN_RSTICK_DOWN)
                                 : (x > 0 ? bindcore::BTN_RSTICK_RIGHT : bindcore::BTN_RSTICK_LEFT);
    } else if (mag < STICK_CENTER) {
        flick_dir = -1;
    }
    for (int b = bindcore::BTN_RSTICK_UP; b <= bindcore::BTN_RSTICK_RIGHT; b++)
        now[b] = flick_dir == b;
}

// Is a chord bound to this action held (its button down since the press
// that picked it)? Hotkey arming lives only while this is true.
inline bool ActionHeld(int action) {
    for (int b = 0; b < bindcore::BTN_COUNT; b++)
        if (prev[b] && latched[b] >= 0 && latched[b] < table.count &&
            table.b[latched[b]].action == action)
            return true;
    return false;
}

inline bool StickIsSelector(unsigned held_mods) {
    return bindcore::StickIsSelector(table, held_mods);
}

// Is this table entry the chord that opens the Quick Menu? quick_menu, or
// quick_chat (End) while the bank-2 hold emits Ctrl (Ctrl+End).
inline bool IsQuickMenuChord(int idx) {
    static const int act_quick_menu = bindcore::FindAction("quick_menu");
    static const int act_quick_chat = bindcore::FindAction("quick_chat");
    if (idx < 0 || idx >= table.count)
        return false;
    const int a = table.b[idx].action;
    return a == act_quick_menu ||
           (a == act_quick_chat && ActionHeld(bindcore::kBank2Action));
}

// One frame: press edges pick chords, holds/one-shots emit keys. `add`
// receives each synthesized scancode.
template <typename AddFn>
inline void Evaluate(const bool now[bindcore::BTN_COUNT], unsigned held_mods,
                     bool menu_mode, AddFn add) {
    using namespace bindcore;
    ctrl_held = false;
    // Quick-menu latch: after the grace window (the menu takes a tick or
    // two to take focus), the first field frame ends it.
    if (quick_menu_open) {
        if (quick_menu_grace > 0)
            quick_menu_grace--;
        else if (!menu_mode) {
            quick_menu_open = false;
            diag::Log("bindings: quick menu closed (focus back to field)");
        }
    }
    const bool cycling = quick_cycle_frames > 0;
    if (cycling) {
        quick_cycle_frames--;
        add(K_RIGHT);
    }
    for (int b = 0; b < BTN_COUNT; b++) {
        if (now[b] && !prev[b]) {
            int idx = Match(table, b, held_mods, menu_mode);
            if (menu_mode && quick_menu_open) {
                // The opening chord again = next tab. Matched in field
                // scope, where the chord lives.
                const int fidx = Match(table, b, held_mods, false);
                if (IsQuickMenuChord(fidx)) {
                    idx = -1;
                    quick_cycle_frames = kOneShotFrames;
                    diag::Log("bindings: quick menu next tab (Right)");
                }
            } else if (!menu_mode && IsQuickMenuChord(idx)) {
                quick_menu_open = true;
                quick_menu_grace = kQuickMenuGraceCalls;
                diag::Log("bindings: quick menu opened - chord again = next tab, "
                          "stick keeps turning");
            }
            latched[b] = idx;
            if (idx >= 0) {
                const Binding& bd = table.b[idx];
                const ActionDef& ad = kActions[bd.action];
                if (ad.mode == MODE_ONESHOT) {
                    if (IsHotkey(bd.action) && hotkey_press != nullptr &&
                        hotkey_press(bd.action, ad.keys[0])) {
                        // armed by the controller - no key press now
                    } else {
                        shot_frames[b] = kOneShotFrames;
                        shot_binding[b] = idx;
                    }
                }
                if (ad.mode != MODE_HOLD) {
                    char chord[kMaxChordText];
                    ChordText(bd, chord, sizeof(chord));
                    diag::Log("bindings: %s (%s)%s", ad.name, chord,
                              menu_mode ? " [menu]" : "");
                }
                if (ad.mode == MODE_RECENTER)
                    PsobbvrVrRecenter();
            }
        }
        if (!now[b])
            latched[b] = -1;
        if (latched[b] >= 0 && kActions[table.b[latched[b]].action].mode == MODE_HOLD) {
            const ActionDef& ad = kActions[table.b[latched[b]].action];
            for (int k = 0; k < ad.nkeys; k++) {
                if (ad.keys[k] == K_LCTRL && cycling)
                    continue;  // a bare Right for the quick menu's next tab
                add(ad.keys[k]);
                if (ad.keys[k] == K_LCTRL)
                    ctrl_held = true;
            }
        }
        if (shot_frames[b] > 0 && shot_binding[b] >= 0 &&
            shot_binding[b] < table.count) {
            shot_frames[b]--;
            const ActionDef& ad = kActions[table.b[shot_binding[b]].action];
            for (int k = 0; k < ad.nkeys; k++)
                add(ad.keys[k]);
        }
        prev[b] = now[b];
    }
}

}  // namespace bindings
