#pragma once

// The controller binding table: which button or grip + button chord
// performs which action. Shared by the in-game mapper (psobbvr_bindings.hpp)
// and the options program (mod/bindings_editor): the action list, input
// tokens, default keymap, chord grammar and conflict rules. Touches
// neither the game nor Windows.
//
// Ini grammar (psobbvr.ini [bindings], one line per action):
//   action = chord[, chord ...]        e.g. hotkey3 = left_grip + x
//   action = none                      explicitly unbound
// A chord is one button, optionally preceded by grip modifiers joined
// with '+'; case-insensitive, spaces ignored. Actions the section does not
// set (or sets badly) keep their defaults (kActions).
//
// Rules (each rejection is logged):
//   - triggers are not bindable (hardcoded in psobbvr_controller.hpp);
//   - a right-stick flick needs a grip modifier (bare, it is the turn);
//   - a hotkey chord cannot use the bank-2 grip (Ctrl) as its only
//     modifier - the game skips its number-key hotkey pass (0x68A720)
//     while any keyboard modifier is down, so it could never fire;
//   - a grip used as a modifier anywhere cannot also have a bare binding,
//     except bank2 (a held Ctrl composes with everything);
//   - two actions on one chord with overlapping scope: a user line beats
//     a default, and between user lines the later one loses.
// At a press, among chords whose modifiers are all held: most non-bank2
// modifiers wins, then most modifiers, then the earlier table entry. So
// "a" still fires with the right grip down (the game makes Ctrl+End =
// Quick Menu), and with both grips down "left_grip+b" (hotkey 2) beats
// "right_grip+b" (menu). Ctrl swaps only the three button actions; the
// ten hotkeys are one row, so both grips + a hotkey chord sends nothing
// the game acts on.

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace bindcore {

// DIK scancodes the actions synthesize.
constexpr unsigned char K_ESC = 0x01, K_1 = 0x02, K_2 = 0x03, K_3 = 0x04,
                        K_4 = 0x05, K_5 = 0x06, K_6 = 0x07, K_7 = 0x08,
                        K_8 = 0x09, K_9 = 0x0A, K_0 = 0x0B, K_TAB = 0x0F,
                        K_ENTER = 0x1C, K_LCTRL = 0x1D, K_SPACE = 0x39,
                        K_HOME = 0xC7,
                        K_UP = 0xC8, K_LEFT = 0xCB, K_RIGHT = 0xCD,
                        K_END = 0xCF, K_DOWN = 0xD0;

// Physical inputs a chord can name. Grips are both buttons and the only
// modifiers. Right-stick flicks are buttons derived from the stick.
enum Button : int {
    BTN_A = 0, BTN_B, BTN_X, BTN_Y,
    BTN_LEFT_GRIP, BTN_RIGHT_GRIP,
    BTN_LEFT_STICK_CLICK, BTN_RIGHT_STICK_CLICK,
    BTN_LEFT_MENU, BTN_RIGHT_MENU,
    BTN_RSTICK_UP, BTN_RSTICK_DOWN, BTN_RSTICK_LEFT, BTN_RSTICK_RIGHT,
    BTN_COUNT
};
constexpr int BTN_MENU_ANY = 100;  // the "menu" token: either hand's menu button
constexpr unsigned MOD_LEFT_GRIP = 1u, MOD_RIGHT_GRIP = 2u;

inline bool IsGrip(int b) { return b == BTN_LEFT_GRIP || b == BTN_RIGHT_GRIP; }
inline bool IsStickFlick(int b) { return b >= BTN_RSTICK_UP && b <= BTN_RSTICK_RIGHT; }
inline unsigned GripMod(int b) {
    return b == BTN_LEFT_GRIP ? MOD_LEFT_GRIP
         : b == BTN_RIGHT_GRIP ? MOD_RIGHT_GRIP : 0u;
}

struct InputToken {
    const char* name;
    int button;
    const char* label;
};
constexpr InputToken kInputs[] = {
    {"a", BTN_A, "A (right hand)"},
    {"b", BTN_B, "B (right hand)"},
    {"x", BTN_X, "X (left hand)"},
    {"y", BTN_Y, "Y (left hand)"},
    {"left_grip", BTN_LEFT_GRIP, "left grip"},
    {"right_grip", BTN_RIGHT_GRIP, "right grip"},
    {"left_stick_click", BTN_LEFT_STICK_CLICK, "left stick click"},
    {"right_stick_click", BTN_RIGHT_STICK_CLICK, "right stick click"},
    {"menu", BTN_MENU_ANY, "menu button (either hand)"},
    {"left_menu", BTN_LEFT_MENU, "left menu button"},
    {"right_menu", BTN_RIGHT_MENU, "right menu button"},
    {"right_stick_up", BTN_RSTICK_UP, "right stick flick up"},
    {"right_stick_right", BTN_RSTICK_RIGHT, "right stick flick right"},
    {"right_stick_down", BTN_RSTICK_DOWN, "right stick flick down"},
    {"right_stick_left", BTN_RSTICK_LEFT, "right stick flick left"},
};
constexpr int kInputCount = sizeof(kInputs) / sizeof(kInputs[0]);

// HOLD = keys down while the chord is held. ONESHOT = a kOneShotFrames
// press per chord press, for keys the game re-triggers while held (Tab,
// hotkeys). RECENTER = VR recenter on the press edge, no key.
enum Mode : int { MODE_HOLD = 0, MODE_ONESHOT = 1, MODE_RECENTER = 2 };
constexpr int kOneShotFrames = 3;  // ~1.5 game ticks (two mapper calls per tick)
// Where an action applies; decided at the press edge, held to release.
enum Scope : int { SCOPE_FIELD = 1, SCOPE_MENU = 2, SCOPE_BOTH = 3 };

struct ActionDef {
    const char* name;
    unsigned char keys[2];
    int nkeys;
    Mode mode;
    int scope;
    const char* default_chords;  // "" = unbound by default
    const char* label;           // options-program / user-guide text
};

// Hotkeys are SCOPE_BOTH: the Customize menu assigns a slot by pressing
// its number key. The menu buttons are unbound by default (on a Quest they
// open the Steam and Meta overlays), so the game menu is a grip chord.
// Sticks and triggers are hardcoded in psobbvr_controller.hpp. Order is
// the tie-break.
constexpr ActionDef kActions[] = {
    {"menu", {K_HOME, 0}, 1, MODE_HOLD, SCOPE_BOTH, "right_grip+b",
     "Game menu open/close (Home)"},
    {"confirm", {K_ENTER, 0}, 1, MODE_HOLD, SCOPE_BOTH, "right_stick_click",
     "Confirm (Enter)"},
    {"cancel", {K_ESC, 0}, 1, MODE_HOLD, SCOPE_BOTH, "left_stick_click",
     "Cancel / close (Esc)"},
    {"recenter", {0, 0}, 0, MODE_RECENTER, SCOPE_BOTH,
     "right_grip+left_stick_click", "Recenter the view and floor"},
    {"bank2", {K_LCTRL, 0}, 1, MODE_HOLD, SCOPE_BOTH, "right_grip",
     "Hold: Ctrl (alternate trigger slots)"},
    {"quick_chat", {K_END, 0}, 1, MODE_HOLD, SCOPE_FIELD, "a",
     "Quick chat (End); with bank2 held = Quick Menu"},
    {"quick_menu", {K_LCTRL, K_END}, 2, MODE_HOLD, SCOPE_FIELD, "",
     "Quick Menu on its own button (Ctrl+End)"},
    {"tab", {K_TAB, 0}, 1, MODE_ONESHOT, SCOPE_BOTH, "y",
     "Tab (item details / chat context)"},
    // The chat line opens on Space (Enter only sends). One-shot: the
    // game re-opens the line while Space is held.
    {"chat", {K_SPACE, 0}, 1, MODE_ONESHOT, SCOPE_FIELD, "right_grip+x",
     "Open the chat line (Space)"},
    {"hotkey1", {K_1, 0}, 1, MODE_ONESHOT, SCOPE_BOTH, "left_grip+a",
     "Hotkey 1 (palette slot 4)"},
    {"hotkey2", {K_2, 0}, 1, MODE_ONESHOT, SCOPE_BOTH, "left_grip+b",
     "Hotkey 2 (palette slot 5)"},
    {"hotkey3", {K_3, 0}, 1, MODE_ONESHOT, SCOPE_BOTH, "left_grip+x",
     "Hotkey 3 (palette slot 6)"},
    {"hotkey4", {K_4, 0}, 1, MODE_ONESHOT, SCOPE_BOTH, "left_grip+y",
     "Hotkey 4 (palette slot 7)"},
    {"hotkey5", {K_5, 0}, 1, MODE_ONESHOT, SCOPE_BOTH,
     "left_grip+right_stick_up", "Hotkey 5 (palette slot 8)"},
    {"hotkey6", {K_6, 0}, 1, MODE_ONESHOT, SCOPE_BOTH,
     "left_grip+right_stick_right", "Hotkey 6 (palette slot 9)"},
    {"hotkey7", {K_7, 0}, 1, MODE_ONESHOT, SCOPE_BOTH,
     "left_grip+right_stick_down", "Hotkey 7 (palette slot 10)"},
    {"hotkey8", {K_8, 0}, 1, MODE_ONESHOT, SCOPE_BOTH,
     "left_grip+right_stick_left", "Hotkey 8 (palette slot 11)"},
    {"hotkey9", {K_9, 0}, 1, MODE_ONESHOT, SCOPE_BOTH,
     "left_grip+right_stick_click", "Hotkey 9 (palette slot 12)"},
    {"hotkey0", {K_0, 0}, 1, MODE_ONESHOT, SCOPE_BOTH,
     "left_grip+left_stick_click", "Hotkey 0 (palette slot 13)"},
    // Unbound by default: the sticks already move the menu cursor.
    {"nav_left", {K_LEFT, 0}, 1, MODE_HOLD, SCOPE_MENU, "",
     "Menus only: Left Arrow"},
    {"nav_right", {K_RIGHT, 0}, 1, MODE_HOLD, SCOPE_MENU, "",
     "Menus only: Right Arrow"},
    {"nav_up", {K_UP, 0}, 1, MODE_HOLD, SCOPE_MENU, "",
     "Menus only: Up Arrow"},
    {"nav_down", {K_DOWN, 0}, 1, MODE_HOLD, SCOPE_MENU, "",
     "Menus only: Down Arrow"},
};
constexpr int kActionCount = sizeof(kActions) / sizeof(kActions[0]);
constexpr int kBank2Action = 4;  // index of "bank2" above

inline bool IsHotkey(int action) {
    return strncmp(kActions[action].name, "hotkey", 6) == 0;
}

inline int FindAction(const char* name) {
    for (int i = 0; i < kActionCount; i++)
        if (_stricmp(kActions[i].name, name) == 0)
            return i;
    return -1;
}

inline int FindInput(const char* token) {
    for (int i = 0; i < kInputCount; i++)
        if (_stricmp(kInputs[i].name, token) == 0)
            return kInputs[i].button;
    return -1;
}

inline const char* ButtonName(int button) {
    for (int i = 0; i < kInputCount; i++)
        if (kInputs[i].button == button)
            return kInputs[i].name;
    return "?";
}

struct Binding {
    int action;
    int button;
    unsigned mods;
    int priority;   // user line index (0 = first) or 1000 + action index
    bool from_user;
};
constexpr int kMaxBindings = 96;
constexpr int kMaxChordText = 96;

struct Table {
    Binding b[kMaxBindings];
    int count = 0;
    unsigned bank2_mods = MOD_RIGHT_GRIP;  // the grip that holds Ctrl
    int accepted_lines = 0, rejected_lines = 0;
    bool section_present = false;
};

using LogFn = void (*)(void* ctx, const char* line);

inline void Logf(LogFn log, void* ctx, const char* fmt, ...) {
    if (log == nullptr)
        return;
    char line[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    log(ctx, line);
}

// "left_grip+a" style text for a binding.
inline void ChordText(const Binding& b, char* out, size_t cap) {
    out[0] = '\0';
    if (b.mods & MOD_LEFT_GRIP)
        strncat_s(out, cap, "left_grip+", _TRUNCATE);
    if (b.mods & MOD_RIGHT_GRIP)
        strncat_s(out, cap, "right_grip+", _TRUNCATE);
    strncat_s(out, cap, ButtonName(b.button), _TRUNCATE);
}

// Parse one chord ("left_grip + x"). Returns false with a reason. The
// "menu" token yields BTN_MENU_ANY; the caller expands it to both hands.
inline bool ParseChord(const char* text, int& button, unsigned& mods,
                       char* err, size_t errlen) {
    button = -1;
    mods = 0;
    char buf[kMaxChordText];
    int n = 0;
    for (const char* p = text; *p != '\0' && n < (int)sizeof(buf) - 1; p++)
        if (!isspace((unsigned char)*p))
            buf[n++] = (char)tolower((unsigned char)*p);
    buf[n] = '\0';
    if (n == 0) {
        snprintf(err, errlen, "empty chord");
        return false;
    }
    int grips = 0, others = 0;
    unsigned grip_mask = 0;
    int other_button = -1;
    char* ctx = nullptr;
    for (char* tok = strtok_s(buf, "+", &ctx); tok != nullptr;
         tok = strtok_s(nullptr, "+", &ctx)) {
        if (strcmp(tok, "left_trigger") == 0 || strcmp(tok, "right_trigger") == 0 ||
            strcmp(tok, "trigger") == 0) {
            snprintf(err, errlen, "'%s' is not bindable (the triggers are the "
                     "palette / confirm / back, fixed)", tok);
            return false;
        }
        const int b = FindInput(tok);
        if (b < 0) {
            snprintf(err, errlen, "unknown input '%s'", tok);
            return false;
        }
        if (IsGrip(b)) {
            if (grip_mask & GripMod(b)) {
                snprintf(err, errlen, "'%s' repeated", tok);
                return false;
            }
            grip_mask |= GripMod(b);
            grips++;
        } else {
            if (others > 0) {
                snprintf(err, errlen, "a chord has one button (found '%s' and "
                         "'%s')", ButtonName(other_button), tok);
                return false;
            }
            other_button = b;
            others++;
        }
    }
    if (others == 1) {
        button = other_button;
        mods = grip_mask;
    } else if (grips == 1) {
        button = grip_mask == MOD_LEFT_GRIP ? BTN_LEFT_GRIP : BTN_RIGHT_GRIP;
        mods = 0;
    } else {
        snprintf(err, errlen, "a chord needs one button (grips alone: one grip)");
        return false;
    }
    if (IsStickFlick(button) && mods == 0) {
        snprintf(err, errlen, "a right-stick flick needs a grip modifier "
                 "(bare, the stick is the smooth turn)");
        return false;
    }
    return true;
}

// Parse "chord, chord" / "none" for one action, appending bindings.
// Returns false (nothing appended) if any chord is bad.
inline bool ParseChordList(const char* value, int action, int priority,
                           bool from_user, Binding* out, int& count,
                           char* err, size_t errlen) {
    char buf[256];
    strncpy_s(buf, value, _TRUNCATE);
    // "none" / empty = unbound.
    {
        char* p = buf;
        while (isspace((unsigned char)*p)) p++;
        char* e = p + strlen(p);
        while (e > p && isspace((unsigned char)e[-1])) *--e = '\0';
        if (*p == '\0' || _stricmp(p, "none") == 0)
            return true;
    }
    Binding tmp[8];
    int n = 0;
    char* ctx = nullptr;
    for (char* tok = strtok_s(buf, ",", &ctx); tok != nullptr;
         tok = strtok_s(nullptr, ",", &ctx)) {
        int button = -1;
        unsigned mods = 0;
        if (!ParseChord(tok, button, mods, err, errlen))
            return false;
        const int expand = button == BTN_MENU_ANY ? 2 : 1;
        for (int k = 0; k < expand; k++) {
            if (n >= (int)(sizeof(tmp) / sizeof(tmp[0]))) {
                snprintf(err, errlen, "too many chords on one action (max 4)");
                return false;
            }
            tmp[n].action = action;
            tmp[n].button = button == BTN_MENU_ANY
                                ? (k == 0 ? BTN_LEFT_MENU : BTN_RIGHT_MENU)
                                : button;
            tmp[n].mods = mods;
            tmp[n].priority = priority;
            tmp[n].from_user = from_user;
            n++;
        }
    }
    for (int i = 0; i < n; i++) {
        if (count >= kMaxBindings) {
            snprintf(err, errlen, "binding table full");
            return false;
        }
        out[count++] = tmp[i];
    }
    return true;
}

// The rules pass: bank-2-only hotkey chords, bare bindings on modifier
// grips, duplicate chords (earlier wins). Compacts the table in place.
inline void DropPass(Table& t, LogFn log, void* ctx) {
    unsigned modifier_grips = 0;
    for (int i = 0; i < t.count; i++)
        modifier_grips |= t.b[i].mods;
    bool drop[kMaxBindings] = {};
    for (int i = 0; i < t.count; i++) {
        const Binding& bi = t.b[i];
        char ci[kMaxChordText];
        ChordText(bi, ci, sizeof(ci));
        if (IsHotkey(bi.action) && bi.mods != 0 && bi.mods == t.bank2_mods) {
            Logf(log, ctx, "dropped '%s = %s': that grip is bank 2 (Ctrl) and "
                 "cannot be a hotkey chord's only modifier - the game ignores "
                 "hotkeys while Ctrl is held; use the other grip",
                 kActions[bi.action].name, ci);
            drop[i] = true;
            continue;
        }
        if (IsGrip(bi.button) && bi.mods == 0 && bi.action != kBank2Action &&
            (modifier_grips & GripMod(bi.button)) != 0) {
            Logf(log, ctx, "dropped '%s = %s': that grip is a chord modifier "
                 "elsewhere, so a bare binding on it would fire with every "
                 "chord (only bank2 may sit bare on a modifier grip)",
                 kActions[bi.action].name, ci);
            drop[i] = true;
            continue;
        }
        for (int j = 0; j < i; j++) {
            if (drop[j])
                continue;
            const Binding& bj = t.b[j];
            if (bj.button != bi.button || bj.mods != bi.mods ||
                (kActions[bj.action].scope & kActions[bi.action].scope) == 0)
                continue;
            if (bj.action == bi.action) {
                drop[i] = true;  // duplicate chord on one action: silent
                break;
            }
            Logf(log, ctx, "dropped '%s = %s' (%s): the chord already belongs "
                 "to '%s' (%s)",
                 kActions[bi.action].name, ci, bi.from_user ? "ini" : "default",
                 kActions[bj.action].name, bj.from_user ? "ini" : "default");
            drop[i] = true;
            break;
        }
    }
    int w = 0;
    for (int i = 0; i < t.count; i++)
        if (!drop[i])
            t.b[w++] = t.b[i];
    t.count = w;
}

// Build the table from the [bindings] section's "key=value" lines (nlines
// = 0: no section) on top of the defaults, applying the rules above.
// Every accepted / rejected line and dropped binding goes to `log`.
inline void Build(const char* const* lines, int nlines, Table& t, LogFn log,
                  void* ctx) {
    t = Table();
    t.section_present = nlines > 0;
    bool user_set[kActionCount] = {};
    char err[192];
    for (int i = 0; i < nlines; i++) {
        const char* line = lines[i];
        const char* eq = strchr(line, '=');
        if (eq == nullptr) {
            Logf(log, ctx, "rejected '%s': not action=chord", line);
            t.rejected_lines++;
            continue;
        }
        char key[64];
        const size_t klen = (size_t)(eq - line) < sizeof(key) - 1
                                ? (size_t)(eq - line) : sizeof(key) - 1;
        memcpy(key, line, klen);
        key[klen] = '\0';
        for (char* e = key + strlen(key); e > key && isspace((unsigned char)e[-1]);)
            *--e = '\0';
        char* k = key;
        while (isspace((unsigned char)*k)) k++;
        const int action = FindAction(k);
        if (action < 0) {
            Logf(log, ctx, "rejected '%s': unknown action '%s'", line, k);
            t.rejected_lines++;
            continue;
        }
        Binding parsed[8];
        int pn = 0;
        if (!ParseChordList(eq + 1, action, i, true, parsed, pn, err, sizeof(err))) {
            Logf(log, ctx, "rejected '%s': %s (keeping the default '%s = %s')",
                 line, err, kActions[action].name,
                 kActions[action].default_chords[0] ? kActions[action].default_chords
                                                    : "none");
            t.rejected_lines++;
            continue;
        }
        if (user_set[action]) {
            // A repeated action line: the later one loses.
            Logf(log, ctx, "rejected '%s': '%s' already set by an earlier line",
                 line, kActions[action].name);
            t.rejected_lines++;
            continue;
        }
        user_set[action] = true;
        t.accepted_lines++;
        for (int j = 0; j < pn && t.count < kMaxBindings; j++)
            t.b[t.count++] = parsed[j];
        Logf(log, ctx, "accepted '%s'", line);
    }
    // Defaults for everything the section did not set.
    for (int a = 0; a < kActionCount; a++) {
        if (user_set[a] || kActions[a].default_chords[0] == '\0')
            continue;
        Binding parsed[8];
        int pn = 0;
        if (ParseChordList(kActions[a].default_chords, a, 1000 + a, false, parsed,
                           pn, err, sizeof(err)))
            for (int j = 0; j < pn && t.count < kMaxBindings; j++)
                t.b[t.count++] = parsed[j];
    }
    // Which grip is bank 2 (Ctrl)? The bare grip binding of "bank2".
    t.bank2_mods = 0;
    for (int i = 0; i < t.count; i++)
        if (t.b[i].action == kBank2Action && t.b[i].mods == 0 && IsGrip(t.b[i].button))
            t.bank2_mods |= GripMod(t.b[i].button);
    DropPass(t, log, ctx);
    // A user line that lost every chord to the rules falls back to the
    // action's default (and the rules run again).
    bool added = false;
    for (int a = 0; a < kActionCount; a++) {
        if (!user_set[a] || kActions[a].default_chords[0] == '\0')
            continue;
        bool any = false;
        for (int i = 0; i < t.count && !any; i++)
            any = t.b[i].action == a;
        if (any)
            continue;
        Binding parsed[8];
        int pn = 0;
        if (!ParseChordList(kActions[a].default_chords, a, 1000 + a, false, parsed, pn,
                            err, sizeof(err)))
            continue;
        // "none" was the user's explicit wish: nothing to restore.
        if (pn == 0)
            continue;
        bool explicit_none = false;
        for (int i = 0; i < nlines && !explicit_none; i++) {
            const char* eq = strchr(lines[i], '=');
            if (eq == nullptr)
                continue;
            char key[64];
            const size_t klen = (size_t)(eq - lines[i]) < sizeof(key) - 1
                                    ? (size_t)(eq - lines[i]) : sizeof(key) - 1;
            memcpy(key, lines[i], klen);
            key[klen] = '\0';
            for (char* e = key + strlen(key); e > key && isspace((unsigned char)e[-1]);)
                *--e = '\0';
            const char* k = key;
            while (isspace((unsigned char)*k)) k++;
            if (FindAction(k) != a)
                continue;
            const char* v = eq + 1;
            while (isspace((unsigned char)*v)) v++;
            explicit_none = *v == '\0' || _strnicmp(v, "none", 4) == 0;
        }
        if (explicit_none)
            continue;
        Logf(log, ctx, "'%s' lost every chord to the rules - back to its default '%s'",
             kActions[a].name, kActions[a].default_chords);
        for (int j = 0; j < pn && t.count < kMaxBindings; j++)
            t.b[t.count++] = parsed[j];
        added = true;
    }
    if (added)
        DropPass(t, log, ctx);
}

// The chord for a button press with the given held modifiers and context;
// -1 = nothing bound. Resolution as in the file comment.
inline int Match(const Table& t, int button, unsigned held_mods, bool menu_mode) {
    int best = -1, best_score = -1;
    for (int i = 0; i < t.count; i++) {
        const Binding& b = t.b[i];
        if (b.button != button || (b.mods & held_mods) != b.mods)
            continue;
        if ((kActions[b.action].scope & (menu_mode ? SCOPE_MENU : SCOPE_FIELD)) == 0)
            continue;
        int bits = 0, other = 0;
        for (unsigned m = b.mods, bit = 1u; m != 0; m >>= 1, bit <<= 1) {
            if (m & 1u) {
                bits++;
                if ((bit & t.bank2_mods) == 0)
                    other++;
            }
        }
        const int score = other * 16 + bits;
        if (score > best_score) {
            best = i;
            best_score = score;
        }
    }
    return best;
}

// True when a flick chord's modifiers are all held: the stick selects
// hotkeys instead of turning.
inline bool StickIsSelector(const Table& t, unsigned held_mods) {
    if (held_mods == 0)
        return false;
    for (int i = 0; i < t.count; i++)
        if (IsStickFlick(t.b[i].button) && t.b[i].mods != 0 &&
            (t.b[i].mods & held_mods) == t.b[i].mods)
            return true;
    return false;
}

// The chords bound to one action, as ini text ("a, left_grip+x" or "none").
inline void ActionChordsText(const Table& t, int action, char* out, size_t cap) {
    out[0] = '\0';
    bool any = false, menu_l = false, menu_r = false;
    for (int i = 0; i < t.count; i++) {
        const Binding& b = t.b[i];
        if (b.action != action)
            continue;
        // Collapse the two hands' menu buttons back to "menu".
        if (b.mods == 0 && b.button == BTN_LEFT_MENU) { menu_l = true; continue; }
        if (b.mods == 0 && b.button == BTN_RIGHT_MENU) { menu_r = true; continue; }
        char c[kMaxChordText];
        ChordText(b, c, sizeof(c));
        if (any)
            strncat_s(out, cap, ", ", _TRUNCATE);
        strncat_s(out, cap, c, _TRUNCATE);
        any = true;
    }
    if (menu_l || menu_r) {
        if (any)
            strncat_s(out, cap, ", ", _TRUNCATE);
        strncat_s(out, cap, menu_l && menu_r ? "menu" : menu_l ? "left_menu" : "right_menu",
                  _TRUNCATE);
        any = true;
    }
    if (!any)
        strncpy_s(out, cap, "none", _TRUNCATE);
}

}  // namespace bindcore
