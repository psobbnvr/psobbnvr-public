#pragma once

// Controller -> game input. Each frame this reads the VR controller state
// and:
//  - pushes the bound keys as synthetic keyboard state into the dinput8
//    proxy (PsobbvrSetSyntheticKeys). The game's actions read the keyboard
//    device; its pad record is only a mirror, so writing there does nothing;
//  - feeds the sticks to the movement module (analog walk: the game caps
//    at 64 = walk / 128 = run, so deflection picks walk or run);
//  - scales the turn rate from right-stick deflection (squared curve
//    between controller_turn_min/max).
//
// Keys mean the same thing everywhere and the game routes them by context,
// like a real keyboard (arrows are both palette and menu navigation). Only
// right-stick Y is mode-gated (Up/Down while a menu is open). The left
// stick always walks - the game allows walking with a menu open.
//
// Buttons go through the binding table (psobbvr_bindings.hpp: a built-in
// keymap, overridable per action from the ini's [bindings] section). Only
// sticks and triggers are hardcoded here:
//   right stick X     smooth turn; Left/Right Arrow in menus (except the
//                     Quick Menu, where it keeps turning); hotkeys 5-8
//                     while the left grip is held
//   right stick Y     Up/Down Arrow in menus
//   right trigger     bottom palette slot / Confirm in menus
//   left trigger      left palette slot / Back in menus
//   both triggers     right palette slot
//
// With a melee weapon the regular attack (right trigger) arms its palette
// key and the physical swing fires it (SwingDetect). Heavy and special
// fire on the chord press so the charge runs; the charge hold then waits
// for the swing (swing_attack=1 arms all three instead). Guns and bare
// hands press directly. With a twin weapon either hand's swing counts
// (LeftSwingActive).

#include <cmath>
#include <windows.h>

#include "psobbvr_bindings.hpp"
#include "psobbvr_gunfire.hpp"
#include "psobbvr_movement.hpp"
#include "psobbvr_stereo.hpp"
#include "psobbvr_vr.hpp"
#include "psobbvr_vrkeyboard.hpp"
#include "psobbvr_weapongrip.hpp"

namespace controller {

// DIK scancodes.
constexpr BYTE K_BACKSPACE = 0x0E, K_ENTER = 0x1C, K_LCTRL = 0x1D,
               K_UP = 0xC8, K_LEFT = 0xCB, K_RIGHT = 0xCD, K_DOWN = 0xD0;

using SetKeysFn = void (*)(const unsigned char*, int);
inline SetKeysFn set_keys_fn = nullptr;
inline bool set_keys_resolved = false;
// Triggers. In the field they are the palette: right = bottom (Down),
// left = left (Left), both = right (Right). A fresh press waits
// TRIG_CHORD_SETTLE OnFrame calls (two per game tick) so a two-trigger
// chord can form before the single-trigger action fires. In menus: right
// = Enter, left = Backspace. The role is fixed at press time and held to
// release, so a confirm that closes the menu does not turn the held
// trigger into an attack.
//
// The both-trigger chord forms only when the second trigger arrives while
// the first is young (<= trig_pair_window_s); a trigger switch with a
// brief overlap fires nothing until the old trigger releases. After a
// special, both triggers are spent, so releasing one finger at a time
// cannot fire a heavy (the regular attack is exempt).
//
// Palette guard: those rules only apply when the slot holds an item or
// tech. For an attack or empty slot, late
// escalation and one-finger de-escalation are free. An uncalibrated arrow
// or unreadable palette falls back to the strict rules.
constexpr int TRIG_CHORD_SETTLE = 3;  // ~1.5 game ticks (~50 ms)
inline bool prev_rtrig = false, rtrig_menu = false;
inline bool prev_ltrig = false, ltrig_menu = false;
inline int trig_settle = TRIG_CHORD_SETTLE;
inline bool prev_rt_field = false, prev_lt_field = false;  // press edges
inline LONGLONG rt_down_qpc = 0, lt_down_qpc = 0;  // field-press times
inline bool rt_spent = false, lt_spent = false;    // consumed by a special
inline BYTE trig_active = 0;   // active palette key this hold; 0 = none
inline LONGLONG trig_qpf = 0;  // QPC frequency (SwingSample's does not
                               // start for guns/bare hands)

inline void PushKeys(const BYTE* keys, int count) {
    if (!set_keys_resolved) {
        set_keys_resolved = true;
        HMODULE dinput = GetModuleHandleA("dinput8.dll");
        if (dinput != nullptr)
            set_keys_fn = reinterpret_cast<SetKeysFn>(
                GetProcAddress(dinput, "PsobbvrSetSyntheticKeys"));
    }
    if (set_keys_fn != nullptr)
        set_keys_fn(keys, count);
}

// Radial deadzone: 0 inside, then rescaled 0..1 to full deflection.
inline float DeadzoneCurve(float v, float dz) {
    const float a = fabsf(v);
    if (a <= dz)
        return 0.0f;
    const float t = (a - dz) / (1.0f - dz);
    return v < 0 ? -(t > 1.0f ? 1.0f : t) : (t > 1.0f ? 1.0f : t);
}

// The game's UI-focus state: u16 at [[[0xA98478]+0x10]+0x1E]. 0x42 =
// normal play; 0x43 = a menu or dialog, 0x40 = word select, 0x4A = the
// teleporter's destination dialog. The middle pointer moves on area
// change, so the chain is re-walked every read. Anything but 0x42 counts
// as focused; stray arrows are harmless where they mean nothing.
constexpr uintptr_t MENU_STATE_BASE_ADDR = 0x00A98478;
inline bool GameUiFocused() {
    if (!diag::Accessible(MENU_STATE_BASE_ADDR, 4, false))
        return false;
    const uintptr_t a =
        *reinterpret_cast<const uintptr_t*>(MENU_STATE_BASE_ADDR);
    if (a == 0 || !diag::Accessible(a + 0x10, 4, false))
        return false;
    const uintptr_t b = *reinterpret_cast<const uintptr_t*>(a + 0x10);
    if (b == 0 || !diag::Accessible(b + 0x1E, 2, false))
        return false;
    return *reinterpret_cast<const uint16_t*>(b + 0x1E) != 0x42;
}

// Is the command palette showing? [0xA98478] is the HUD root; its
// per-player HUD controllers sit at +0x54 + 4 * slot (vtable 0xB3FA90),
// and the controller's +0x38 is the palette window (vtable 0xB3F0E0). The
// window's u16 at +8 holds bit 0x10 = not drawn (set / cleared every tick
// by the game's show / hide helpers 0x712714 / 0x7127C8) and bit 0x04 =
// not yet revealed (cleared ~38 ticks after creation, never in lobbies).
// Showing = neither bit set; no window = not showing.
constexpr uintptr_t HUD_ROOT_ADDR = 0x00A98478;    // -> the HUD root object
constexpr uintptr_t HUD_LOCAL_SLOT_ADDR = 0x00A9C4F4;  // local player slot (= gamecam::ENTITY_INDEX_ADDR)
constexpr uintptr_t HUD_ROOT_CTRL_OFF = 0x54;        // + 4 * slot -> HUD controller
constexpr uintptr_t HUD_CTRL_VTABLE = 0x00B3FA90;    // per-player HUD controller
constexpr uintptr_t HUD_CTRL_PALETTE_OFF = 0x38;     // -> the three-button palette window
constexpr uintptr_t PALETTE_WINDOW_VTABLE = 0x00B3F0E0;
constexpr uint16_t WINDOW_GATE_NOT_DRAWN = 0x10;    // u16 at window+8
constexpr uint16_t WINDOW_GATE_UNREVEALED = 0x04;
constexpr uintptr_t WINDOW_STATE_OFF = 0x2C;         // u32: bit 1 open, bit 2 sliding

struct PaletteWindow {
    uintptr_t root = 0, ctrl = 0, win = 0;
    uint16_t gate = 0;   // window+8
    uint32_t state = 0;  // window+0x2C
    bool visible = false;
};

// False when the chain is missing (no HUD, controller or window yet).
inline bool ReadPaletteWindow(PaletteWindow& p) {
    p = PaletteWindow{};
    if (!diag::Accessible(HUD_ROOT_ADDR, 4, false) ||
        !diag::Accessible(HUD_LOCAL_SLOT_ADDR, 4, false))
        return false;
    const uintptr_t root = *reinterpret_cast<const uintptr_t*>(HUD_ROOT_ADDR);
    const uint32_t slot = *reinterpret_cast<const uint32_t*>(HUD_LOCAL_SLOT_ADDR);
    if (root == 0 || slot >= 12)
        return false;
    p.root = root;
    const uintptr_t ctrl_at = root + HUD_ROOT_CTRL_OFF + 4 * slot;
    if (!diag::Accessible(ctrl_at, 4, false))
        return false;
    const uintptr_t ctrl = *reinterpret_cast<const uintptr_t*>(ctrl_at);
    if (ctrl == 0 || !diag::Accessible(ctrl, 0x40, false) ||
        *reinterpret_cast<const uintptr_t*>(ctrl) != HUD_CTRL_VTABLE)
        return false;
    p.ctrl = ctrl;
    const uintptr_t win = *reinterpret_cast<const uintptr_t*>(ctrl + HUD_CTRL_PALETTE_OFF);
    if (win == 0 || !diag::Accessible(win, 0x30, false) ||
        *reinterpret_cast<const uintptr_t*>(win) != PALETTE_WINDOW_VTABLE)
        return false;
    p.win = win;
    p.gate = *reinterpret_cast<const uint16_t*>(win + 8);
    p.state = *reinterpret_cast<const uint32_t*>(win + WINDOW_STATE_OFF);
    p.visible = (p.gate & (WINDOW_GATE_NOT_DRAWN | WINDOW_GATE_UNREVEALED)) == 0;
    return true;
}

inline bool PaletteVisible() {
    PaletteWindow p;
    return ReadPaletteWindow(p) && p.visible;
}

// Modal prompts (e.g. the guild card "accept?" box) leave the UI-focus
// value at 0x42 (normal play), so they are detected by the HUD
// controller's +0x10, which holds the prompt window while one is up.
// Nonzero = menu mode. Logged once per change.
constexpr uintptr_t HUD_CTRL_MODAL_OFF = 0x10;
inline bool GameModalPrompt() {
    static bool prev = false;
    bool now = false;
    if (diag::Accessible(HUD_ROOT_ADDR, 4, false) && diag::Accessible(HUD_LOCAL_SLOT_ADDR, 4, false)) {
        const uintptr_t root = *reinterpret_cast<const uintptr_t*>(HUD_ROOT_ADDR);
        const uint32_t slot = *reinterpret_cast<const uint32_t*>(HUD_LOCAL_SLOT_ADDR);
        if (root != 0 && slot < 12) {
            const uintptr_t ctrl_at = root + HUD_ROOT_CTRL_OFF + 4 * slot;
            if (diag::Accessible(ctrl_at, 4, false)) {
                const uintptr_t ctrl = *reinterpret_cast<const uintptr_t*>(ctrl_at);
                if (ctrl != 0 && diag::Accessible(ctrl, 0x40, false) &&
                    *reinterpret_cast<const uintptr_t*>(ctrl) == HUD_CTRL_VTABLE)
                    now = *reinterpret_cast<const uintptr_t*>(ctrl + HUD_CTRL_MODAL_OFF) != 0;
            }
        }
    }
    if (now != prev) {
        prev = now;
        diag::Log("uifocus: modal prompt %s (HUD controller +0x10) - stick %s",
                  now ? "UP" : "gone", now ? "navigates it" : "back to locomotion");
    }
    return now;
}

// Per-hand speed sampling: grip speed in m/s in tracking space (the
// physical hand speed, unaffected by recenter). BeginScene runs twice per
// game tick on the same pose, so only pose changes are sampled, timed by
// QPC. Feeds the swing detector and the 'swingtrace' log. Hand 1 (right)
// samples in the swing modes; hand 0 (left) with a twin weapon or when it
// may cast (cast_left_hand).
struct HandSwing {
    float prev_pos[3] = {};
    bool prev_valid = false;
    LONGLONG prev_qpc = 0;       // time of the latest sample
    bool fresh = false;          // this OnFrame call produced a new sample
    float speed = 0.0f;          // the sample (m/s)
    int above = 0;               // consecutive samples >= onset while armed
    bool swinging = false;       // fired; re-arms once below swing_release
    LONGLONG last_fire_qpc = 0;  // this hand's refractory clock
};
inline HandSwing sw_hand[2];     // [0] left, [1] right
inline LONGLONG st_qpf = 0;

// Armed-swing state. The settled trigger chord arms a palette key; a
// swing of either sampled hand fires it (one press, one stamp).
inline BYTE sw_armed_key = 0;    // this frame's armed palette key; 0 = none
// Stamp-only arming: the detector records the swing but presses no key.
// Used while our charged heavy/special runs - its release and the row
// hold's strikes wait on the stamp.
constexpr BYTE SW_KEY_STAMP = 0xFF;
inline bool sw_armed_melee = false;  // a melee chord, not a tech (the
                                     // left hand fires these with a twin
                                     // weapon)
inline bool sw_armed_tech_left = false;  // a technique the left hand casts
                                         // too (TechLeftCasts)
inline LONGLONG sw_fire_qpc = 0; // last fire time (the swing stamp)
inline int sw_fire_hand = 1;     // the hand that made the last fire
inline int sw_fire_frames = 0;   // OnFrame calls left of the synthetic press
inline int sw_gap_frames = 0;    // forced-release calls after the press
inline BYTE sw_fire_key = 0;     // the key being synthesized

// The left hand's swing counts with a twin weapon (one half per hand):
// daggers, knuckles and twin swords, by the same kind rules the twin
// weapon split uses. A double saber is one staff and stays right-hand.
// [vr] swing_left_hand.
inline bool LeftSwingActive() {
    return vrmod::config.swing_left_hand &&
           weapongrip::current_weapon != nullptr &&
           !weapongrip::current_is_gun &&
           (weapongrip::KindSplitsFunnel(weapongrip::current_kind) ||
            weapongrip::KindSplitsComposite(weapongrip::current_kind));
}

inline void SwingSample(int hand, const vrmod::ControllerState& cs,
                        bool want_detect) {
    HandSwing& h = sw_hand[hand];
    h.fresh = false;
    constexpr bool trace = false;
    if (!trace && !want_detect) {
        h.prev_valid = false;
        return;
    }
    D3DMATRIX pose;
    if (!vrmod::Get()->GetHandPose(hand, pose)) {
        h.prev_valid = false;
        return;
    }
    const float ws = vrmod::config.world_scale;
    const float p[3] = {pose._41 / ws, pose._42 / ws, pose._43 / ws};
    if (h.prev_valid && p[0] == h.prev_pos[0] && p[1] == h.prev_pos[1] &&
        p[2] == h.prev_pos[2])
        return;  // second BeginScene of the tick - same located pose
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    if (st_qpf == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        st_qpf = f.QuadPart;
    }
    if (h.prev_valid) {
        const double dt = double(qpc.QuadPart - h.prev_qpc) / double(st_qpf);
        if (dt > 0.0005 && dt < 0.5) {
            const float dx = p[0] - h.prev_pos[0], dy = p[1] - h.prev_pos[1],
                        dz = p[2] - h.prev_pos[2];
            h.speed = (float)(sqrtf(dx * dx + dy * dy + dz * dz) / dt);
            h.fresh = true;
        }
    }
    h.prev_pos[0] = p[0];
    h.prev_pos[1] = p[1];
    h.prev_pos[2] = p[2];
    h.prev_qpc = qpc.QuadPart;
    h.prev_valid = true;
}

// The swing detector, per hand. swing_confirm consecutive samples >=
// swing_onset while armed (outside the refractory window) fire the armed
// key as a ~2-tick press followed by at least one released tick (the game
// wants edges). After a fire the hand must slow below swing_release
// before it can fire again. Samples above swing_glitch_cap are tracking
// pose snaps (they arrive flagged TRACKED) and are ignored without
// resetting the count. Each hand has its own hysteresis and refractory.
inline void SwingDetectHand(int hand) {
    HandSwing& h = sw_hand[hand];
    if (sw_armed_key == 0)
        h.above = 0;  // swinging survives disarm: a re-fire still
                      // needs the slow-down below release
    if (!h.fresh)
        return;
    const auto& c = vrmod::config;
    const char* hn = hand == 0 ? "left" : "right";
    if (h.speed >= c.swing_glitch_cap) {
        if (sw_armed_key != 0)
            diag::Log("swingattack: glitch frame discarded (%.1f m/s, %s)",
                      h.speed, hn);
    } else if (h.speed >= c.swing_onset) {
        if (sw_armed_key != 0) {
            h.above++;
            const bool refractory_ok =
                st_qpf > 0 &&
                double(h.prev_qpc - h.last_fire_qpc) / double(st_qpf) >=
                    c.swing_refractory_s;
            if (!h.swinging && h.above >= c.swing_confirm &&
                sw_gap_frames == 0 && refractory_ok) {
                h.swinging = true;
                h.last_fire_qpc = h.prev_qpc;
                sw_fire_qpc = h.prev_qpc;
                sw_fire_hand = hand;
                sw_fire_key = sw_armed_key;
                sw_fire_frames = 4;  // ~2 game ticks held
                diag::Log("swingattack: FIRE key 0x%02X at %.2f m/s (%s)%s",
                          sw_fire_key, h.speed, hn,
                          sw_fire_key == SW_KEY_STAMP ? " (stamp only)" : "");
                if (sw_fire_key == K_DOWN)  // a normal attack: decide
                    gunfire::MeleeSnapFire();  // the facing snap
            }
        }
    } else {
        h.above = 0;
        if (h.speed < c.swing_release)
            h.swinging = false;
    }
}
inline void SwingDetect(bool left_active) {
    SwingDetectHand(1);
    // The left hand fires melee chords with a twin weapon, and techniques
    // it does not aim (an aimed one, e.g. Foie, is cast by the right hand
    // while the left points it).
    if ((left_active && sw_armed_melee) || sw_armed_tech_left)
        SwingDetectHand(0);
    else
        sw_hand[0].above = 0;
}

// The attack action state machine:
//   [entity+0x32E] word = action mode (1 idle; 5/6/7 = attack modes
//     sharing the per-tick stage machine 0x698FA8).
//   [entity+0x330] word = stage. Heavy/special enter at stage 1
//     (0x6A2AD4; variant from 0x6A2E8C, 0/1/2 = normal/heavy/special),
//     stage 2 is the charge countdown, stage 3 the swing (damage and
//     target sampling). Normals enter directly at stage 3.
//   [entity+0x8BC] int = the stage's tick countdown: 6 at attack start,
//     decremented only in stage 2; dropping below 0 enters stage 3.
//     Every stage entry rewrites it, so a held value never goes stale.
constexpr uintptr_t ENT_ACTION_MODE_OFF = 0x32E;
constexpr uintptr_t ENT_ACTION_STAGE_OFF = 0x330;
constexpr uintptr_t ENT_STAGE_TIMER_OFF = 0x8BC;
// Countdown floor while holding: topped up only when the game's decrement
// goes below it, so the normal charge duration is kept. Once top-up stops
// the swing fires within ~2 ticks.
constexpr int CHARGE_HOLD_TICKS = 2;
// The hold only claims a charge our chord press started within this
// window; a keyboard heavy/special charges and fires natively.
constexpr double CH_OURS_WINDOW_S = 0.5;

inline bool ch_holding = false;   // topping up the timer this stage
inline bool ch_done = false;      // released; don't re-engage this stage
inline int ch_above[2] = {};      // consecutive onset samples per hand (release)
inline LONGLONG ch_start_qpc = 0; // engage time (timeout)
inline bool ch_ours = false;      // charge initiated by OUR trigger chord
inline LONGLONG ch_release_qpc = 0;  // when a swing released our charge
                                     // (0 = none); read by SwingWarp
inline BYTE ch_cancel_key = 0;    // the chord a timeout cancelled: no
                                  // press-through until it is released
// The game's action dispatcher 0x698A08 (thiscall on the entity: mode,
// cmd). Mode 1 cmd 0 = the idle-mode setup 0x6A1C8C, which ends the
// running action through its end handler (the melee attack's 0x6A2DC4
// stops the charge effect and clears the pause bit) and plays idle - the
// path a knockback takes. Used to cancel an attack so nothing fires.
// __fastcall stands in for __thiscall (edx is a dummy).
constexpr uintptr_t FN_ACTION_DISPATCH = 0x698A08;
using ActionDispatchFn = void(__fastcall*)(void* entity, void* edx, int mode, int cmd);
constexpr int ACTION_MODE_IDLE = 1;
inline bool ch_was_charging = false;  // attack-entry edge detector
inline LONGLONG chord_press_qpc = 0;  // last synthesized K_LEFT/K_RIGHT

// The charge hold (swing_attack mode 2): the heavy/special chord presses
// its key and the game starts the charge; this keeps the stage-2
// countdown at a floor so the attack waits charged until the physical
// swing. Then top-up stops, the countdown expires and stage 3 runs
// normally. Interrupts (knockback, death, area change) leave the mode or
// stage, which disengages the hold with no cleanup. Only charges our
// chord started are held (CH_OURS_WINDOW_S).
inline void ChargeHold(bool gameplay_drives) {
    const auto& c = vrmod::config;
    bool want = false;
    bool charging = false;
    uintptr_t entity = 0;
    short mode = 0;
    if (c.charge_hold && c.swing_attack == 2 && gameplay_drives &&
        weapongrip::SwingWeapon()) {
        entity = gamecam::ResolveEntity();
        if (entity != 0 &&
            diag::Accessible(entity + ENT_ACTION_MODE_OFF, 4, true) &&
            diag::Accessible(entity + ENT_STAGE_TIMER_OFF, 4, true)) {
            mode = *reinterpret_cast<short*>(entity + ENT_ACTION_MODE_OFF);
            const short stage =
                *reinterpret_cast<short*>(entity + ENT_ACTION_STAGE_OFF);
            charging = mode >= 5 && mode <= 7 && (stage == 1 || stage == 2);
            want = mode >= 5 && mode <= 7 && stage == 2;
        }
    }
    // Attack entry: ours if our chord key (stamped every frame it is
    // down) was pressed within the window.
    if (charging && !ch_was_charging) {
        LARGE_INTEGER en;
        QueryPerformanceCounter(&en);
        ch_ours = trig_qpf > 0 && chord_press_qpc != 0 &&
                  double(en.QuadPart - chord_press_qpc) / double(trig_qpf) <=
                      CH_OURS_WINDOW_S;
        if (!ch_ours)
            diag::Log("chargehold: PASS mode %d (not our press - native "
                      "charge)", (int)mode);
    } else if (!charging) {
        ch_ours = false;
    }
    ch_was_charging = charging;
    if (!want || !ch_ours) {
        // Not in a held charge: re-arm for the next.
        ch_holding = ch_done = false;
        ch_above[0] = ch_above[1] = 0;
        return;
    }
    if (ch_done)
        return;  // released; the countdown is running out on its own
    // Release detection with the swing detector's thresholds, per hand;
    // the left hand counts with a twin weapon.
    for (int hand = 1; hand >= 0; hand--) {
        const HandSwing& h = sw_hand[hand];
        if (hand == 0 && !LeftSwingActive()) {
            ch_above[0] = 0;
            continue;
        }
        if (!h.fresh)
            continue;
        if (h.speed >= c.swing_glitch_cap) {
            // Tracking pose-snap glitch: neither advance nor reset.
        } else if (h.speed >= c.swing_onset) {
            ch_above[hand]++;
        } else {
            ch_above[hand] = 0;
        }
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!ch_holding) {
        ch_holding = true;
        ch_above[0] = ch_above[1] = 0;
        ch_start_qpc = now.QuadPart;
        diag::Log("chargehold: ENGAGE mode %d", (int)mode);
    }
    const double held_s = st_qpf > 0
        ? double(now.QuadPart - ch_start_qpc) / double(st_qpf)
        : 0.0;
    const int swung_hand = ch_above[1] >= c.swing_confirm ? 1
                         : ch_above[0] >= c.swing_confirm ? 0 : -1;
    const bool swung = swung_hand >= 0;
    if (swung) {
        ch_done = true;
        ch_holding = false;
        // SwingWarp paces a swing-released charge (reads the stamp).
        ch_release_qpc = now.QuadPart;
        diag::Log("chargehold: RELEASE (%s, %.2f m/s, held %.2f s)",
                  swung_hand == 0 ? "left swing" : "swing",
                  sw_hand[swung_hand].speed, held_s);
    } else if (c.charge_hold_timeout_s > 0.0f &&
               held_s >= c.charge_hold_timeout_s) {
        // No swing came: cancel the attack (nothing fires) and keep the
        // held chord from starting another until it is released.
        ch_done = true;
        ch_holding = false;
        ch_release_qpc = 0;
        ch_cancel_key = trig_active;
        reinterpret_cast<ActionDispatchFn>(FN_ACTION_DISPATCH)(
            reinterpret_cast<void*>(entity), nullptr, ACTION_MODE_IDLE, 0);
        diag::Log("chargehold: TIMEOUT after %.2f s - the charge is "
                  "cancelled, nothing fires (release the chord to charge "
                  "again)", held_s);
    } else {
        // Top-up only: the countdown runs 6..2 untouched, then pins.
        int* cd = reinterpret_cast<int*>(entity + ENT_STAGE_TIMER_OFF);
        if (*cd < CHARGE_HOLD_TICKS)
            *cd = CHARGE_HOLD_TICKS;
    }
}

// Stage 3 (the swing) runs three routines per tick: 0x699058 hits,
// 0x699384 sound/step, 0x69941C exit. The hit fires on the tick the
// animation clock (float +0xC0, advanced by the step +0xC8) crosses the
// weapon's damage frame - the crossing test 0x7AAA30, gated off during an
// animation blend (+0xB8 != +0xBA). Crossed = frame < clock <= frame +
// step, with no stored previous value, so a clock held above the frame
// never re-fires and one held at or below frame - step never fires. The
// damage frame comes from 0x6AC48C (cdecl, arg = entity), keyed by weapon
// kind, combo step +0x8B4, hit counter +0x8B2 and the +0x964 flag. The
// advance 0x7AA094 ends a one-shot clip (+0xF4 == 3) when clock >= length
// (+0xC4) - 1, setting +0xB0 |= 0x30 for the exit decider, so holding the
// clock delays the clip end and the combo chain by the ticks held.
constexpr uintptr_t ENT_ANIM_CUR_OFF = 0xB8;    // word: current anim id
constexpr uintptr_t ENT_ANIM_NEXT_OFF = 0xBA;   // word: queued anim id
constexpr uintptr_t ENT_ANIM_CLOCK_OFF = 0xC0;  // float: animation clock
constexpr uintptr_t ENT_ANIM_STEP_OFF = 0xC8;   // float: clock step/tick
constexpr uintptr_t ENT_ATK_VARIANT_OFF = 0x8A4;  // WORD: 0/1/2 = n/h/s
// Read +0x8A4 as a word: +0x8A6 behind it is where the combo-accept
// decider 0x6A31B0 stashes the next step's variant, and it stays stale
// after the chain ends (an int read would misread fresh normals as
// charged after a heavy-ended chain).
constexpr uintptr_t FN_MELEE_DAMAGE_FRAME = 0x6AC48C;
using DamageFrameFn = float(__cdecl*)(uintptr_t entity);
// The warp only claims attacks our swing started - a normal fired by the
// swing's press, or a charge our hold released on a swing
// (ch_release_qpc) - within this window of the stamp.
constexpr double WP_OURS_WINDOW_S = 0.5;

inline bool wp_done = false;           // decision made for this attack
inline bool wp_was_attacking = false;  // stage-3 presence last frame
inline int wp_combo_step = -1;         // +0x8B4 at decision (combo re-arm)
inline float wp_prev_clock = -1.0f;    // last observed clock (restart det.)
inline bool wp_warping = false;        // maintaining the warp write
inline float wp_target = 0.0f;         // the maintained warp-to value
inline int wp_delay = 0;               // frames before the first write:
                                       // the swing animation starts a
                                       // tick after stage 3 (0x7AA4C8
                                       // sets +0xB8 and +0xBA together
                                       // and zeroes the clock), so an
                                       // entry-frame write is wiped
inline int wp_writes = 0;              // maintain-write count (1 = the
                                       // write stuck; large = the game
                                       // rewrites +0xC0 each tick)
inline bool wp_ours_paced = false;     // current attack is ours: paced
                                       // by the warp and the holds
inline int wp_variant = 0;             // its variant at entry (the
                                       // combo-window stretch is
                                       // normals only)
inline bool wp_stretch_prev = false;   // window-open state last frame
inline bool wp_stretched = false;      // bonus applied this attack step
// The post-hit hold and the multi-hit row hold (swing_row_hold). The hit
// counter +0x8B2 changing means a hit landed this tick (0x699058
// increments it per crossing; guns count +0x8B8 and never warp). After a
// step's last hit the clock is paused for the ticks the warp skipped, so
// hits keep the original game's pace; after an earlier hit of a
// multi-hit step the clock runs on to just under the next strike's frame
// and waits there for the next swing or the cap.
inline int wp_skipped_ticks = 0;       // ticks the warp removed this step
inline short wp_hit_ctr = 0;           // +0x8B2 last frame (hit detection)
inline int wp_hold = 0;                // 0 none, 1 post-hit hold,
                                       // 2 row run-up, 3 row hold
inline float wp_hold_value = 0.0f;     // the maintained clock value
inline int wp_hold_left = 0;           // post-hit: advances still to undo
inline int wp_hold_ticks = 0;          // advances undone so far
inline float wp_row_frame = 0.0f;      // row run-up: the next strike's frame
// The swing-fire stamp already consumed (the swing that fired the attack
// or last released a hold). A newer stamp is an unspent swing and
// releases the next hold at once, even one that has not begun yet.
inline LONGLONG wp_consumed_qpc = 0;
inline bool SwingPending() { return sw_fire_qpc != wp_consumed_qpc; }
// A swing that releases a hold is spent, and OnFrame suppresses its
// press: the combo scanner 0x6A1088 would take it as the next-step input
// (accepted if the window is open, or poison - bit 0x04, no later press
// chains - before it opens). So only a swing after a step's last strike
// chains the next step.
inline void SwingHoldSpend() { wp_consumed_qpc = sw_fire_qpc; }
inline bool SwingHoldWaiting() { return wp_hold == 2 || wp_hold == 3; }
inline const char* SwingHandName() {
    return sw_fire_hand == 0 ? "left swing" : "swing";
}
// Game ticks: the game polls the keyboard once per tick through our
// dinput8 proxy, which counts the polls (PsobbvrKeyboardPolls). A paused
// clock cannot be watched for ticks, so the holds count polls. Without
// the export: wall clock at 30 ticks/s.
using PollsFn = unsigned long (*)();
inline PollsFn polls_fn = nullptr;
inline bool polls_resolved = false;
inline unsigned long GameTicks() {
    if (!polls_resolved) {
        polls_resolved = true;
        HMODULE dinput = GetModuleHandleA("dinput8.dll");
        if (dinput != nullptr)
            polls_fn = reinterpret_cast<PollsFn>(
                GetProcAddress(dinput, "PsobbvrKeyboardPolls"));
        if (polls_fn == nullptr)
            diag::Log("swinghold: PsobbvrKeyboardPolls export missing - "
                      "holds count wall-clock ticks (30/s)");
    }
    if (polls_fn != nullptr)
        return polls_fn();
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (unsigned long)(t.QuadPart * 30 / f.QuadPart);
}
// Holds use the game's pause bit (+0xB0 bit 0x40): the clock advance
// (0x7AA094) returns at once and the effects tick (0x699384), which runs
// root motion, is skipped - so velocity (+0x30C/+0x314) is zeroed once
// and the character stands still. The crossing test (0x7AAA30) ignores
// the bit, so a post-hit pause first steps the clock past the crossing
// window (state 1 below).
inline bool wp_paused = false;          // we set the pause bit
inline uintptr_t wp_hold_entity = 0;    // the entity it was set on
inline unsigned long wp_hold_tick0 = 0; // GameTicks() at the pause
inline void SwingHoldPause(uintptr_t entity) {
    *reinterpret_cast<uint32_t*>(entity + 0xB0) |= 0x40u;
    *reinterpret_cast<float*>(entity + 0x30C) = 0.0f;
    *reinterpret_cast<float*>(entity + 0x314) = 0.0f;
    wp_paused = true;
    wp_hold_entity = entity;
    wp_hold_tick0 = GameTicks();
}
inline void SwingHoldUnpause() {
    if (wp_paused && wp_hold_entity != 0 &&
        diag::Accessible(wp_hold_entity + 0xB0, 4, true))
        *reinterpret_cast<uint32_t*>(wp_hold_entity + 0xB0) &= ~0x40u;
    wp_paused = false;
}
inline void SwingHoldReset(const char* why) {
    if (wp_hold != 0)
        diag::Log("swinghold: dropped (%s) in state %d after %d ticks",
                  why, wp_hold, wp_hold_ticks);
    SwingHoldUnpause();
    wp_hold = 0;
    wp_hold_left = 0;
    wp_hold_ticks = 0;
    wp_skipped_ticks = 0;
}
// The equipped weapon's kind, read like the game's getter 0x687670: the
// dword at +8 of the weapon object [entity+0x38C], 0 when >= 0x13.
// Multi-hit kinds: 3 dagger, 0xE double saber, 0xF twin sword. -1 = no
// weapon object or unreadable.
inline int MeleeKind(uintptr_t entity) {
    if (!diag::Accessible(entity + 0x38C, 4, true))
        return -1;
    const uintptr_t w = *reinterpret_cast<uintptr_t*>(entity + 0x38C);
    if (w == 0 || !diag::Accessible(w + 8, 4, true))
        return -1;
    const unsigned k = *reinterpret_cast<unsigned*>(w + 8);
    return k < 0x13 ? (int)k : 0;
}
// Is the equipped weapon multi-hit? Asked of the game's damage-frame
// getter rather than a kind list (rares can have kinds of their own): the
// multi-hit tables are indexed by the hit counter +0x8B2, the general one
// ignores it. Reads rows 0 and 1 (counter set and restored in place) and
// compares.
inline bool MultiHitWeapon(uintptr_t entity) {
    short* ctr = reinterpret_cast<short*>(entity + 0x8B2);
    const short saved = *ctr;
    *ctr = 0;
    const float row0 =
        reinterpret_cast<DamageFrameFn>(FN_MELEE_DAMAGE_FRAME)(entity);
    *ctr = 1;
    const float row1 =
        reinterpret_cast<DamageFrameFn>(FN_MELEE_DAMAGE_FRAME)(entity);
    *ctr = saved;
    return row0 > 0.0f && row1 != row0;
}

// The swing warp: a swing-fired attack starts stage 3 at clock zero, so
// its wind-up would play after the physical swing and the hit land late.
// Once any blend settles, jump the clock to (damage frame - margin *
// step) and keep writing it while the clock is below that; the game's
// crossing test then fires the hit mid-swing. The first hit's forward
// step is skipped with the wind-up. Chained combo steps re-enter stage 3
// with no visible gap, so a new attack is also recognised by the combo
// step +0x8B4 changing or the clock jumping backward.
inline void SwingWarp(bool gameplay_drives) {
    auto& c = vrmod::config;
    bool attack_mode = false;
    uintptr_t entity = 0;
    short mode = 0, stage = 0;
    if (c.swing_attack == 2 && gameplay_drives && weapongrip::SwingWeapon()) {
        entity = gamecam::ResolveEntity();
        if (entity != 0 &&
            diag::Accessible(entity + ENT_ACTION_MODE_OFF, 4, true) &&
            diag::Accessible(entity + ENT_ANIM_CUR_OFF, 0x14, true) &&
            diag::Accessible(entity + ENT_ATK_VARIANT_OFF, 0x20, true) &&
            diag::Accessible(entity + 0x964, 4, true) &&
            diag::Accessible(entity + 0xF4, 4, true) &&
            diag::Accessible(entity + 0x30C, 0xC, true) &&
            diag::Accessible(entity + 4, 4, true)) {
            mode = *reinterpret_cast<short*>(entity + ENT_ACTION_MODE_OFF);
            stage = *reinterpret_cast<short*>(entity + ENT_ACTION_STAGE_OFF);
            attack_mode = mode >= 5 && mode <= 7;
        }
    }
    const bool attacking = attack_mode && stage == 3;
    if (!attacking) {
        if (wp_warping)
            diag::Log("swingwarp: END clock %.2f after %d writes",
                      wp_prev_clock, wp_writes);
        SwingHoldReset("attack over");
        if (wp_paused)  // never leave the game's clip paused
            SwingHoldUnpause();
        wp_done = false;
        wp_was_attacking = false;
        wp_warping = false;
        wp_combo_step = -1;
        wp_prev_clock = -1.0f;
        wp_ours_paced = false;
        wp_variant = 0;
        wp_stretch_prev = false;
        wp_stretched = false;
        return;
    }
    const float clock_now =
        *reinterpret_cast<float*>(entity + ENT_ANIM_CLOCK_OFF);
    const int combo_step = *reinterpret_cast<int*>(entity + 0x8B4);
    // New attack? A fresh stage-3 entry, a combo-step change, or the
    // clock jumping backward while we are not writing it.
    const bool restarted =
        wp_was_attacking &&
        (combo_step != wp_combo_step ||
         (!wp_warping && wp_hold == 0 && wp_prev_clock >= 0.0f &&
          clock_now < wp_prev_clock - 0.001f));
    if (!wp_was_attacking || restarted) {
        if (wp_warping)
            diag::Log("swingwarp: END clock %.2f after %d writes (chain)",
                      wp_prev_clock, wp_writes);
        SwingHoldReset(restarted ? "chain" : "entry");
        wp_hit_ctr = *reinterpret_cast<short*>(entity + 0x8B2);
        wp_was_attacking = true;
        wp_combo_step = combo_step;
        wp_done = false;
        wp_warping = false;
        // Ours? A normal (variant 0): our swing-fired press within the
        // window. A heavy/special (1/2): our charge hold released it on
        // a swing within the window.
        LARGE_INTEGER en;
        QueryPerformanceCounter(&en);
        const int variant =
            *reinterpret_cast<short*>(entity + ENT_ATK_VARIANT_OFF);
        const bool charged = variant != 0;
        const bool fresh_stamp =
            !charged && trig_qpf > 0 && sw_fire_key == K_DOWN &&
            sw_fire_qpc != 0 &&
            double(en.QuadPart - sw_fire_qpc) / double(trig_qpf) <=
                WP_OURS_WINDOW_S;
        const bool fresh_release =
            charged && trig_qpf > 0 && ch_release_qpc != 0 &&
            double(en.QuadPart - ch_release_qpc) / double(trig_qpf) <=
                WP_OURS_WINDOW_S;
        // A chained normal step is ours if the step before it was (the
        // holds push its start past the stamp window). A charged step
        // is judged by its own release stamp, first or chained.
        const bool chained = restarted && combo_step > 0;
        const bool ours = charged ? fresh_release
                        : chained ? wp_ours_paced : fresh_stamp;
        wp_ours_paced = ours;
        wp_variant = variant;
        wp_stretched = false;
        // The firing swing is spent - except on a chained charged step,
        // where the release swing also lands the step's first strike.
        if (!chained && !(charged && combo_step > 0))
            wp_consumed_qpc = sw_fire_qpc;
        if (!ours) {
            wp_done = true;  // native attack: never touch it
            diag::Log("swingwarp: PASS %s (step %d%s)",
                      charged ? "native charge" : "native press",
                      combo_step, restarted ? ", chain" : "");
        } else {
            wp_delay = 2;  // ~1 game tick: let the swing anim start
        }
    }
    wp_prev_clock = clock_now;
    // Combo-window stretch (combo_window_bonus, our normals only): when
    // the window opens, add the bonus once to the follow-through
    // countdown. The bit drops between steps, so this re-fires per step.
    // The combo decider 0x6A31B0 reads the lifecycle bits at +0x8AC:
    // 0x01 next step accepted, 0x02 window open (set at the attack's last
    // hit, 0x6A2FB4), 0x04 poison (a press outside the window; then even
    // an in-window press is refused, 0x6A31E1), 0x20 window closed. Open
    // = 0x02 set, 0x24 clear and a next step exists (+0x8B4 below 2).
    {
        const uint32_t bits = *reinterpret_cast<uint32_t*>(entity + 0x8AC);
        const bool open =
            (bits & 0x02) != 0 && (bits & 0x24) == 0 && combo_step < 2;
        if (open && !wp_stretch_prev && !wp_stretched && wp_ours_paced &&
            wp_variant == 0 && c.combo_window_bonus > 0) {
            wp_stretched = true;
            int* cd = reinterpret_cast<int*>(entity + ENT_STAGE_TIMER_OFF);
            const int before = *cd;
            *cd = before + c.combo_window_bonus;
            diag::Log("combowin: +%d ticks at step %d (countdown %d -> %d)",
                      c.combo_window_bonus, combo_step, before, *cd);
        }
        wp_stretch_prev = open;
    }
    float* clock = reinterpret_cast<float*>(entity + ENT_ANIM_CLOCK_OFF);
    // A hit landed this tick? (+0x8B2 changed.) Only our swing-started
    // attacks are paced; native ones keep their clocks.
    bool hold_started = false;  // this frame: the machine below must
                                // not judge it by this frame's earlier
                                // clock read
    {
        const short hit_ctr = *reinterpret_cast<short*>(entity + 0x8B2);
        if (hit_ctr != wp_hit_ctr) {
            wp_hit_ctr = hit_ctr;
            if (wp_warping) {
                // The crossing happened: the warp is over by definition.
                diag::Log("swingwarp: CROSSED clock %.2f after %d writes "
                          "(hit)", clock_now, wp_writes);
                wp_warping = false;
            }
            if (wp_ours_paced) {
                const float step =
                    *reinterpret_cast<float*>(entity + ENT_ANIM_STEP_OFF);
                // The getter reads the NEW hit counter: the next row's
                // frame for a multi-hit kind, 0 for its exhausted rows,
                // the same (already crossed) frame for a one-hit kind.
                const float next =
                    reinterpret_cast<DamageFrameFn>(FN_MELEE_DAMAGE_FRAME)(entity);
                if (c.swing_row_hold && step > 0.0f &&
                    next > clock_now + step) {
                    wp_hold = 2;
                    wp_row_frame = next;
                    wp_hold_ticks = 0;
                    hold_started = true;
                    diag::Log("swinghold: hit %d landed clock %.2f, next "
                              "strike frame %.2f - run-up, then wait for "
                              "the swing (margin %.2f, cap %d%s)",
                              (int)hit_ctr, clock_now, next,
                              c.swing_row_hold_margin, c.swing_row_hold_cap,
                              SwingPending() ? ", a swing is pending" : "");
                } else if (wp_skipped_ticks > 0 && step > 0.0f) {
                    // One step past the crossing window, then pause.
                    // The bump is one tick's advance done early, so the
                    // pause runs one tick longer than the skip.
                    wp_hold = 1;
                    wp_hold_value = clock_now + step;
                    *clock = wp_hold_value;
                    wp_prev_clock = wp_hold_value;
                    wp_hold_left = wp_skipped_ticks + 1;
                    wp_hold_ticks = 0;
                    SwingHoldPause(entity);
                    hold_started = true;
                    diag::Log("swinghold: hit %d landed clock %.2f (last "
                              "of step %d) - paused at %.2f for %d ticks "
                              "(the skipped wind-up)",
                              (int)hit_ctr, clock_now, combo_step,
                              wp_hold_value, wp_hold_left);
                } else {
                    diag::Log("swinghold: hit %d landed clock %.2f (last "
                              "of step %d) - nothing skipped, no hold",
                              (int)hit_ctr, clock_now, combo_step);
                }
            }
        }
    }
    // The hold machine (from the frame after a hold starts). If a paused
    // clock moved, a new clip started under the pause: stand down and
    // clear the bit (the clip start 0x7AA4C8 does not, and the new clip
    // would never advance).
    if (hold_started)
        return;
    if (wp_hold == 1) {
        if (fabsf(clock_now - wp_hold_value) > 0.001f) {
            SwingHoldReset("clock moved while paused");
        } else {
            wp_hold_ticks = (int)(GameTicks() - wp_hold_tick0);
            if (wp_hold_ticks >= wp_hold_left) {
                SwingHoldUnpause();
                diag::Log("swinghold: released clock %.2f after %d ticks",
                          wp_hold_value, wp_hold_ticks);
                wp_hold = 0;
            }
        }
        return;
    }
    if (wp_hold == 2 || wp_hold == 3) {
        const bool swung = SwingPending();
        const float step =
            *reinterpret_cast<float*>(entity + ENT_ANIM_STEP_OFF);
        if (wp_hold == 2) {
            // Run-up at normal pace. A swing during it releases the row
            // (the strike lands at its normal frame, never earlier);
            // otherwise stop just under the frame.
            const float hp =
                wp_row_frame - c.swing_row_hold_margin * step;
            if (swung || c.swing_row_hold_cap <= 0 || !(step > 0.0f)) {
                diag::Log("swinghold: row released during the run-up "
                          "(%s) clock %.2f",
                          swung ? SwingHandName() : "no wait", clock_now);
                if (swung)
                    SwingHoldSpend();
                wp_hold = 0;
            } else if (clock_now > hp) {
                *clock = hp;
                wp_prev_clock = hp;
                wp_hold_value = hp;
                wp_hold_ticks = 0;
                wp_hold = 3;
                SwingHoldPause(entity);
                diag::Log("swinghold: row hold paused at %.2f (strike "
                          "frame %.2f, step %.2f) - waiting for the swing",
                          hp, wp_row_frame, step);
            }
            return;
        }
        if (swung) {
            SwingHoldUnpause();
            diag::Log("swinghold: row released (%s) after %d ticks, "
                      "clock %.2f",
                      SwingHandName(), wp_hold_ticks, clock_now);
            SwingHoldSpend();
            wp_hold = 0;
        } else if (wp_hold_ticks >= c.swing_row_hold_cap) {
            // No swing within the cap: cancel the attack through the
            // idle-mode setup - the strike never lands, the combo ends.
            SwingHoldUnpause();
            reinterpret_cast<ActionDispatchFn>(FN_ACTION_DISPATCH)(
                reinterpret_cast<void*>(entity), nullptr, ACTION_MODE_IDLE, 0);
            diag::Log("swinghold: no swing within %d ticks - the attack "
                      "is cancelled, the strike does not land (clock %.2f)",
                      wp_hold_ticks, clock_now);
            wp_hold = 0;
        } else if (fabsf(clock_now - wp_hold_value) > 0.001f) {
            SwingHoldReset("clock moved while paused");
        } else {
            wp_hold_ticks = (int)(GameTicks() - wp_hold_tick0);
        }
        return;
    }
    if (wp_done && !wp_warping)
        return;
    if (wp_warping) {
        // Maintain until the observed clock reaches the target.
        if (clock_now < wp_target - 0.001f) {
            *clock = wp_target;
            wp_writes++;
            wp_prev_clock = wp_target;
        } else {
            diag::Log("swingwarp: CROSSED clock %.2f after %d writes",
                      clock_now, wp_writes);
            wp_warping = false;
        }
        return;
    }
    // Let the swing animation actually start (see wp_delay), then
    // wait out any blend: while +0xB8 != +0xBA the crossing test is
    // gated and the clock may still be rewritten by the blend-in.
    if (wp_delay > 0) {
        wp_delay--;
        return;
    }
    if (*reinterpret_cast<short*>(entity + ENT_ANIM_CUR_OFF) !=
        *reinterpret_cast<short*>(entity + ENT_ANIM_NEXT_OFF))
        return;
    const float step =
        *reinterpret_cast<float*>(entity + ENT_ANIM_STEP_OFF);
    if (!(step > 0.0f))
        return;  // paused/odd frame: try again next frame
    const float frame =
        reinterpret_cast<DamageFrameFn>(FN_MELEE_DAMAGE_FRAME)(entity);
    // A chained step of a multi-hit weapon: its first strike waits for
    // its own swing - warp to just under the strike's frame and hold
    // like a row hold. A swing already pending lands it at once.
    if (combo_step > 0 && c.swing_row_hold &&
        c.swing_row_hold_cap > 0 && frame > 0.0f &&
        MultiHitWeapon(entity)) {
        const float hp = frame - c.swing_row_hold_margin * step;
        if (SwingPending()) {
            diag::Log("swinghold: chained step %d (kind %d): a %s is "
                      "pending - strike at once",
                      combo_step, MeleeKind(entity), SwingHandName());
            SwingHoldSpend();
        } else if (hp > clock_now) {
            wp_skipped_ticks = (int)((hp - clock_now) / step + 0.5f);
            *clock = hp;
            wp_prev_clock = hp;
            wp_hold_value = hp;
            wp_hold_ticks = 0;
            wp_hold = 3;
            wp_done = true;
            wp_warping = false;
            SwingHoldPause(entity);
            diag::Log("swinghold: chained step %d (kind %d): warp clock "
                      "%.2f -> %.2f (strike frame %.2f, step %.2f, skips "
                      "%d ticks), paused for the swing (cap %d)",
                      combo_step, MeleeKind(entity), clock_now, hp, frame,
                      step, wp_skipped_ticks, c.swing_row_hold_cap);
            return;
        }
    }
    const float warp_to = frame - c.swing_warp_margin * step;
    if (!(frame > 0.0f) || clock_now >= warp_to) {
        // Already at/past the damage frame (or no frame): leave it.
        wp_done = true;
        diag::Log("swingwarp: SKIP clock %.2f frame %.2f (step %.2f)",
                  clock_now, frame, step);
        return;
    }
    // Ticks skipped, given back by the post-hit hold.
    wp_skipped_ticks = (int)((warp_to - clock_now) / step + 0.5f);
    diag::Log("swingwarp: WARP clock %.2f -> %.2f (frame %.2f, step "
              "%.2f, margin %.2f, cstep %d, skips %d ticks)",
              clock_now, warp_to, frame, step, c.swing_warp_margin,
              combo_step, wp_skipped_ticks);
    *clock = warp_to;
    wp_prev_clock = warp_to;
    wp_target = warp_to;
    wp_writes = 1;
    wp_warping = true;
    wp_done = true;
}

// The action palette, read for the chord guard and the context
// passthrough.
//
// It lives on the player entity as two banks of 0x74 bytes, bank 0 at
// +0x538 and bank 1 at +0x5AC: a u32 header (0 = the main 14 entries at
// +0x04 are live, nonzero = the alternate 14 at +0x3C), then entries of
// {u8 type, u8 param, u16 pad}. Bank 1 is used only when [entity+0x524]
// has bit 2 and bank 1's header is nonzero (0x68AA24).
//
// Entry types (executor 0x68A40C / 0x68C190): 0 empty; 1 context action
// (see PaletteSlotForButton); 2 attack (param = 0/1/2 normal/heavy/
// special); 3 technique (param = tech id); 4 item (param = tool code);
// 5 technique via action 9; 6 mag photon blast. Types 0-2 are safe to
// fire by accident; 3+ are guarded.
//
// Button -> slot: button 1 = slot 0 while slot 0's param != 0, else slot
// 1; button 2 = slot 2; button 3 = slot 3; hotkeys = slots 4..13. Each
// fire writes the entry's {type,param} to [entity+0x354]. The Ctrl
// palette-swap modifier swaps which key fires slots 2 and 3; we know our
// own Ctrl, so the swap is mirrored.
//
// Key -> button comes from the key config: button masks at 0xA0F674 (1),
// 0xA0F67C (2), 0xA0F678 (3), compared against the held mask in pad
// record 0 (0xAAE75C). It is calibrated live: while exactly one palette
// arrow is synthesized in the field, the held mask names its button.
constexpr uintptr_t ENT_PAL_FLAGS_OFF = 0x524;  // bit 2: bank 1 eligible
constexpr uintptr_t ENT_PAL_BANK0_OFF = 0x538;
constexpr uintptr_t ENT_PAL_BANK1_OFF = 0x5AC;
constexpr uintptr_t ENT_PAL_LATCH_OFF = 0x354;  // {type,param} last fired
constexpr uintptr_t PAL_KEYMASK_BTN1_ADDR = 0x00A0F674;
constexpr uintptr_t PAL_KEYMASK_BTN2_ADDR = 0x00A0F67C;  // slot 2 unmodified
constexpr uintptr_t PAL_KEYMASK_BTN3_ADDR = 0x00A0F678;  // slot 3 unmodified
constexpr uintptr_t PAL_PAD_RECORD0_ADDR = 0x00AAE75C;   // held mask at +0

// Calibrated game button (1..3) per arrow K_DOWN/K_LEFT/K_RIGHT, seeded
// with the default mapping so the guard works from the first press.
// 0 = unknown (strict rules for that arrow).
inline int pal_btn[3] = {1, 2, 3};

inline int PalArrowIdx(BYTE arrow) {
    return arrow == K_DOWN ? 0
         : arrow == K_LEFT ? 1
         : arrow == K_RIGHT ? 2 : -1;
}

// The live entry array (the active bank's main or alt set); 0 =
// unreadable / no entity.
inline uintptr_t PaletteEntries(uintptr_t entity) {
    if (entity == 0 ||
        !diag::Accessible(entity + ENT_PAL_FLAGS_OFF, 4, false) ||
        !diag::Accessible(entity + ENT_PAL_BANK0_OFF, 0xE8, false))
        return 0;
    uintptr_t bank = entity + ENT_PAL_BANK0_OFF;
    if ((*reinterpret_cast<const uint32_t*>(entity + ENT_PAL_FLAGS_OFF) & 2) &&
        *reinterpret_cast<const uint32_t*>(entity + ENT_PAL_BANK1_OFF) != 0)
        bank = entity + ENT_PAL_BANK1_OFF;
    return bank +
           (*reinterpret_cast<const uint32_t*>(bank) == 0 ? 0x04 : 0x3C);
}

inline bool PaletteEntry(uintptr_t entity, int slot, BYTE& type,
                         BYTE& param) {
    const uintptr_t e = PaletteEntries(entity);
    if (e == 0 || slot < 0 || slot > 13)
        return false;
    type = *reinterpret_cast<const BYTE*>(e + slot * 4);
    param = *reinterpret_cast<const BYTE*>(e + slot * 4 + 1);
    return true;
}

// Mirror of the executor's button -> slot choice (ctrl_swap = our own
// synthesized Ctrl). Slot 0 is the context slot: the game rewrites its
// param every idle tick (0x6892E8) with the locked target's context code
// [target+0x98], and executing it calls the target's vtbl+0x2C (talk /
// pick up / teleport) - so param != 0 means button 1 interacts.
inline int PaletteSlotForButton(uintptr_t entity, int button,
                                bool ctrl_swap) {
    if (button == 2)
        return ctrl_swap ? 3 : 2;
    if (button == 3)
        return ctrl_swap ? 2 : 3;
    BYTE t = 0, p = 0;
    if (PaletteEntry(entity, 0, t, p) && p != 0)
        return 0;  // slot 0 publishes an action - overrides button 1
    return 1;
}

// Self-calibration. Called only while exactly one palette arrow is
// synthesized in the field with no Ctrl.
inline void PaletteCalibrate(BYTE arrow) {
    const int idx = PalArrowIdx(arrow);
    if (idx < 0 || !diag::Accessible(PAL_PAD_RECORD0_ADDR, 4, false) ||
        !diag::Accessible(PAL_KEYMASK_BTN1_ADDR, 4, false) ||
        !diag::Accessible(PAL_KEYMASK_BTN2_ADDR, 4, false) ||
        !diag::Accessible(PAL_KEYMASK_BTN3_ADDR, 4, false))
        return;
    const uint32_t held =
        *reinterpret_cast<const uint32_t*>(PAL_PAD_RECORD0_ADDR);
    const uint32_t m[3] = {
        *reinterpret_cast<const uint32_t*>(PAL_KEYMASK_BTN1_ADDR),
        *reinterpret_cast<const uint32_t*>(PAL_KEYMASK_BTN2_ADDR),
        *reinterpret_cast<const uint32_t*>(PAL_KEYMASK_BTN3_ADDR)};
    int btn = 0;
    for (int i = 0; i < 3; i++) {
        if (m[i] != 0 && (held & m[i]) != 0) {
            if (btn != 0)
                return;  // more than one button matches - ambiguous frame
            btn = i + 1;
        }
    }
    if (btn != 0 && pal_btn[idx] != btn) {
        pal_btn[idx] = btn;
        diag::Log("palette: arrow 0x%02X -> button %d (calibrated)", arrow,
                  btn);
    }
}

// True when firing this arrow's palette action by accident is harmless
// (see the return below). Uncalibrated arrow or unreadable palette =
// false (strict rules).
inline bool PaletteArrowSafe(BYTE arrow, bool ctrl_swap) {
    const int idx = PalArrowIdx(arrow);
    if (idx < 0 || pal_btn[idx] == 0)
        return false;
    const uintptr_t entity = gamecam::ResolveEntity();
    BYTE t = 0, p = 0;
    if (!PaletteEntry(entity,
                      PaletteSlotForButton(entity, pal_btn[idx], ctrl_swap),
                      t, p))
        return false;
    // Types 0/1/2 are always safe (nothing surprise-fires); techs
    // (3/5) are safe while cast arming is on - an armed tech fires
    // nothing until the physical swing. Items (4) stay guarded.
    return t <= 2 || ((t == 3 || t == 5) && vrmod::config.cast_swing);
}

// Technique records: 19 of 0x2C bytes at 0x9CF3C0, indexed by tech id;
// +0x18 = the targeting yaw cone's full angle in BAMS (int32). 0x10000 =
// any direction, 0 = no cone test - both pick their own target.
constexpr uintptr_t TECH_RECORDS_ADDR = 0x009CF3C0;
constexpr uintptr_t TECH_RECORD_STRIDE = 0x2C;
constexpr uintptr_t TECH_REC_YAW_OFF = 0x18;
constexpr unsigned TECH_RECORD_COUNT = 19;

// Does the left-hand ray aim this technique? The tech steering policy
// (psobbvr_targetaim.hpp): tech steering on (target_aim bit 4), listed by
// cast_aim_techs, and a real yaw cone (Foie, Rafoie, Barta, Gibarta,
// Gizonde, Megid). Unknown id or unreadable record = aimed.
inline bool TechAimedByLeftHand(unsigned id) {
    if ((vrmod::config.target_aim & 4) == 0)
        return false;
    const int mask = vrmod::config.cast_aim_techs;
    if (mask >= 0 && (id >= 32 || ((mask >> id) & 1) == 0))
        return false;
    if (id >= TECH_RECORD_COUNT)
        return true;
    const uintptr_t rec = TECH_RECORDS_ADDR + id * TECH_RECORD_STRIDE;
    if (!diag::Accessible(rec + TECH_REC_YAW_OFF, 4, false))
        return true;
    const int32_t yaw = *reinterpret_cast<const int32_t*>(rec + TECH_REC_YAW_OFF);
    return yaw > 0 && yaw < 0x10000;
}

// Does a left-hand swing cast this palette entry? Techniques (types 3/5,
// param = the tech id) the left hand does not aim - support techs and the
// ones that pick their own target ([vr] cast_left_hand).
inline bool TechLeftCasts(BYTE type, BYTE param) {
    return vrmod::config.cast_left_hand && (type == 3 || type == 5) &&
           !TechAimedByLeftHand(param);
}

// TechLeftCasts for a trigger chord's palette slot.
inline bool ArrowTechLeftCasts(BYTE arrow, bool ctrl_swap) {
    const int idx = PalArrowIdx(arrow);
    if (idx < 0 || pal_btn[idx] == 0)
        return false;
    const uintptr_t entity = gamecam::ResolveEntity();
    BYTE t = 0, p = 0;
    return PaletteEntry(entity,
                        PaletteSlotForButton(entity, pal_btn[idx], ctrl_swap),
                        t, p) &&
           TechLeftCasts(t, p);
}

// Which arrow's context passthrough was already logged this hold.
inline BYTE ctx_direct_key = 0;
// Combat-free-area state last frame (logged on change).
inline bool sw_combatfree_prev = false;

// Hotkey-bar arming ([vr] hotkey_arm): the hotkey armed for the swing,
// its arm time, and the last swing fire consumed. The mode flags are
// this frame's context, set in OnFrame before the bindings evaluate.
inline BYTE hk_armed_key = 0;
inline int hk_armed_action = -1;   // the hotkey action whose chord must stay held
inline bool hk_armed_left = false; // its technique casts on a left swing too
inline LONGLONG hk_armed_qpc = 0;
inline LONGLONG hk_seen_fire_qpc = 0;
inline bool hk_swing_mode = false, hk_cast_mode = false, hk_menu_mode = false;

// bindings::hotkey_press: at the chord's press edge, decide whether this
// hotkey arms (tech slot, melee normal attack) or presses through.
// DIK 1..0 (0x02..0x0B) = palette slots 4..13. The Ctrl bank swap's
// effect on hotkey slots is not mapped; the entry read is logged.
inline bool HotkeyPress(int action, unsigned char key) {
    if (!vrmod::config.hotkey_arm || hk_menu_mode ||
        (!hk_swing_mode && !hk_cast_mode))
        return false;
    const int slot = 4 + (int)key - 0x02;
    if (slot < 4 || slot > 13)
        return false;
    BYTE t = 0, p = 0;
    if (!PaletteEntry(gamecam::ResolveEntity(), slot, t, p)) {
        diag::Log("hotkeyarm: slot %d unreadable - direct press", slot);
        return false;
    }
    bool arms = false;
    if (t == 3 || t == 5)
        arms = hk_cast_mode;
    else if (t == 2)
        arms = hk_swing_mode && (vrmod::config.swing_attack == 1 || p == 0);
    if (!arms) {
        diag::Log("hotkeyarm: slot %d type %d param %d - direct press", slot, t, p);
        return false;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    hk_armed_key = key;
    hk_armed_action = action;
    hk_armed_left = TechLeftCasts(t, p);
    hk_armed_qpc = now.QuadPart;
    diag::Log("hotkeyarm: ARMED key 0x%02X (slot %d type %d param %d) - %s",
              key, slot, t, p,
              hk_armed_left ? "swing either hand to fire"
                            : "swing to fire");
    return true;
}

// Does this arrow's palette action arm for the swing rather than press
// through? Attacks (type 2) arm under allow_attack, techs (3/5) under
// allow_tech (any weapon). Context actions, items and empty slots press
// through. Uncalibrated or unreadable = treated as an attack. Every idle
// tick the game copies the locked target's context code [target+0x98]
// into palette slot 0's param (maintainer 0x6892E8), and button 1 runs
// slot 0 (the target's vtbl+0x2C: talk / pick up / teleport) whenever
// that param is nonzero (0x68C160), else slot 1's attack.
inline bool PaletteArrowArms(BYTE arrow, bool ctrl_swap,
                             bool allow_attack, bool allow_tech) {
    const int idx = PalArrowIdx(arrow);
    if (idx < 0 || pal_btn[idx] == 0)
        return allow_attack;
    const uintptr_t entity = gamecam::ResolveEntity();
    BYTE t = 0, p = 0;
    if (!PaletteEntry(entity,
                      PaletteSlotForButton(entity, pal_btn[idx], ctrl_swap),
                      t, p))
        return allow_attack;
    if (t == 2)
        return allow_attack;
    if (t == 3 || t == 5)
        return allow_tech;
    return false;
}


// Called every frame from BeginScene, before movement::OnFrame so the
// turn-rate scale is set when it refreshes the rate. gameplay_drives =
// the camera takeover is driving this frame.
inline void OnFrame(bool gameplay_drives) {
    vrmod::ControllerState cs;
    const bool have =
        vrmod::config.controllers && vrmod::Get()->GetControllerState(cs);
    // On-screen keyboard (psobbvr_vrkeyboard.hpp): detect a text field
    // every frame; while its panel is up the controller belongs to it.
    vrkeyboard::Tick();
    const bool kbd_modal = have && vrkeyboard::Active();
    if (!have || kbd_modal) {
        // No controllers, not focused, or the keyboard owns the frame:
        // everything back to neutral.
        PushKeys(nullptr, 0);
        movement::stick_move_active = false;
        movement::stick_turn_scale = -1.0f;
        movement::stick_raw_active = false;
        movement::stick_raw_side = movement::stick_raw_fwd =
            movement::stick_raw_turn = 0.0f;
        bindings::Reset();
        hk_armed_key = 0;
        prev_rtrig = prev_ltrig = false;
        prev_rt_field = prev_lt_field = false;
        rt_spent = lt_spent = false;
        trig_active = 0;
        trig_settle = TRIG_CHORD_SETTLE;
        // Drop all armed-swing state; nothing stays latched.
        for (HandSwing& h : sw_hand) {
            h.prev_valid = false;
            h.fresh = false;
            h.above = 0;
            h.swinging = false;
        }
        sw_armed_key = 0;
        sw_armed_melee = false;
        sw_armed_tech_left = false;
        sw_fire_frames = sw_gap_frames = 0;
        ch_holding = ch_done = false;
        ch_above[0] = ch_above[1] = 0;
        ch_ours = ch_was_charging = false;
        if (kbd_modal) {
            // The keyboard's own control keys are the only synthesized
            // keys this frame.
            BYTE kk[4];
            int kn = 0;
            auto kadd = [&](BYTE sc) {
                if (kn < (int)sizeof(kk))
                    kk[kn++] = sc;
            };
            vrkeyboard::Drive(cs, kadd);
            PushKeys(kk, kn);
        }
        return;
    }

    // Armed swings apply with a melee weapon in the field while the
    // takeover drives (weapongrip::OnFrame classified the weapon earlier
    // this BeginScene). Lobbies (floor 15) and towns (floor 0) are
    // combat-free, and some of their interactables never enter targeting,
    // so every chord presses directly there.
    const uintptr_t sw_entity = gamecam::ResolveEntity();
    uint32_t sw_floor = 0xFFFFFFFF;
    if (sw_entity != 0 &&
        diag::Accessible(sw_entity + gamecam::ENTITY_FLOOR_OFFSET, 4, false))
        sw_floor = *reinterpret_cast<const uint32_t*>(
            sw_entity + gamecam::ENTITY_FLOOR_OFFSET);
    const bool combat_free = sw_floor == 0 || sw_floor == 15;
    if (combat_free != sw_combatfree_prev) {
        sw_combatfree_prev = combat_free;
        diag::Log("swingattack: %s (floor %u)",
                  combat_free ? "combat-free area - chords press direct"
                              : "combat area - arming live",
                  sw_floor);
    }
    const bool swing_mode = vrmod::config.swing_attack != 0 &&
                            gameplay_drives && !combat_free &&
                            weapongrip::SwingWeapon();
    // Tech chords arm with any weapon, so the detector also samples
    // whenever cast arming could apply. Independent of swing_attack.
    const bool cast_swing_mode = vrmod::config.cast_swing &&
                                 gameplay_drives && !combat_free;
    SwingSample(1, cs, swing_mode || cast_swing_mode);
    // The left hand samples with a twin weapon, and under cast arming
    // when it may cast (sw_armed_tech_left).
    const bool left_mode = swing_mode && LeftSwingActive();
    const bool left_cast_mode = cast_swing_mode && vrmod::config.cast_left_hand;
    SwingSample(0, cs, left_mode || left_cast_mode);

    // Menu mode = a non-gameplay screen, the game's UI focus, a modal
    // prompt, or the shrunken-viewport latch (a fallback for moments the
    // focus chain is unreadable; it only sees the field menu).
    const bool menu_mode =
        !gameplay_drives || GameUiFocused() || GameModalPrompt() ||
        stereo::menu_viewport_active;
    diag::ui_focused = menu_mode;  // the menu-routing trace counts these frames
    const float press = vrmod::config.controller_press;
    const float dz = vrmod::config.controller_deadzone;
    const float side_sign = vrmod::config.controller_side_sign < 0 ? -1.0f : 1.0f;

    BYTE keys[32];
    int n = 0;
    auto add = [&](BYTE sc) {
        if (n < (int)sizeof(keys))
            keys[n++] = sc;
    };

    // Buttons, grips, stick clicks and right-stick flicks go through the
    // binding table. Runs before the triggers so the palette guard knows
    // this frame's Ctrl hold.
    bindings::Poll();
    bindings::hotkey_press = &HotkeyPress;
    hk_swing_mode = swing_mode;
    hk_cast_mode = cast_swing_mode;
    hk_menu_mode = menu_mode;
    unsigned held_mods = 0;
    {
        bool now[bindcore::BTN_COUNT] = {};
        now[bindcore::BTN_A] = cs.primary[1];
        now[bindcore::BTN_B] = cs.secondary[1];
        now[bindcore::BTN_X] = cs.primary[0];
        now[bindcore::BTN_Y] = cs.secondary[0];
        now[bindcore::BTN_LEFT_GRIP] = cs.squeeze[0] > press;
        now[bindcore::BTN_RIGHT_GRIP] = cs.squeeze[1] > press;
        now[bindcore::BTN_LEFT_STICK_CLICK] = cs.stick_click[0];
        now[bindcore::BTN_RIGHT_STICK_CLICK] = cs.stick_click[1];
        now[bindcore::BTN_LEFT_MENU] = cs.menu[0];
        now[bindcore::BTN_RIGHT_MENU] = cs.menu[1];
        bindings::StickFlick(cs.stick_x[1], cs.stick_y[1], !menu_mode, now);
        held_mods = (now[bindcore::BTN_LEFT_GRIP] ? bindcore::MOD_LEFT_GRIP : 0u) |
                    (now[bindcore::BTN_RIGHT_GRIP] ? bindcore::MOD_RIGHT_GRIP : 0u);
        bindings::Evaluate(now, held_mods, menu_mode, add);
    }

    // Triggers: see the comment at TRIG_CHORD_SETTLE.
    const bool rtrig_now = cs.trigger[1] > press;
    const bool ltrig_now = cs.trigger[0] > press;
    if (rtrig_now && !prev_rtrig)
        rtrig_menu = menu_mode;
    if (ltrig_now && !prev_ltrig)
        ltrig_menu = menu_mode;
    prev_rtrig = rtrig_now;
    prev_ltrig = ltrig_now;
    if (rtrig_now && rtrig_menu)
        add(K_ENTER);                      // menu confirm
    if (ltrig_now && ltrig_menu)
        add(K_BACKSPACE);                  // menu back
    const bool rt_field = rtrig_now && !rtrig_menu;
    const bool lt_field = ltrig_now && !ltrig_menu;
    if (trig_qpf == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        trig_qpf = f.QuadPart;
    }
    LARGE_INTEGER tq;
    QueryPerformanceCounter(&tq);
    // Field-press edges: stamp the press time, clear the spent mark.
    // gun_haptic 1 pulses the pulling hand here with a gun; mode 2 (per
    // bullet) is in psobbvr_gunfire.hpp.
    if (rt_field && !prev_rt_field) {
        rt_down_qpc = tq.QuadPart;
        rt_spent = false;
        if (vrmod::config.gun_haptic == 1 && weapongrip::current_is_gun)
            vrmod::Get()->HapticPulse(1, vrmod::config.gun_haptic_s,
                                      vrmod::config.gun_haptic_amp);
    }
    if (lt_field && !prev_lt_field) {
        lt_down_qpc = tq.QuadPart;
        lt_spent = false;
        if (vrmod::config.gun_haptic == 1 && weapongrip::current_is_gun)
            vrmod::Get()->HapticPulse(0, vrmod::config.gun_haptic_s,
                                      vrmod::config.gun_haptic_amp);
    }
    // The special forms at a second-trigger edge: always when its slot
    // is safe, else only if the partner is young and unspent.
    const double pair_w = vrmod::config.trig_pair_window_s;
    auto young = [&](LONGLONG down_qpc) {
        return double(tq.QuadPart - down_qpc) / double(trig_qpf) <= pair_w;
    };
    const bool ctrl_swap = bindings::ctrl_held;  // our synthesized Ctrl (bank2)
    const bool safe_both = PaletteArrowSafe(K_RIGHT, ctrl_swap);
    if ((lt_field && !prev_lt_field && rt_field &&
         (safe_both || (!rt_spent && young(rt_down_qpc)))) ||
        (rt_field && !prev_rt_field && lt_field &&
         (safe_both || (!lt_spent && young(lt_down_qpc))))) {
        trig_active = K_RIGHT;
        rt_spent = lt_spent = true;
    }
    prev_rt_field = rt_field;
    prev_lt_field = lt_field;
    // Deactivate when the active action's chord is gone.
    if ((trig_active == K_RIGHT && !(rt_field && lt_field)) ||
        (trig_active == K_DOWN && !rt_field) ||
        (trig_active == K_LEFT && !lt_field))
        trig_active = 0;
    // Single-trigger activation after the settle window. Right activates
    // spent or not (gun fire resumes after a special); left only unspent
    // or when its slot is safe. Both held with nothing active = a pending
    // switch: no output until one releases.
    if (trig_active == 0 &&
        ((rt_field && !lt_field) ||
         (lt_field && !rt_field &&
          (!lt_spent || PaletteArrowSafe(K_LEFT, ctrl_swap))))) {
        if (trig_settle > 0)
            trig_settle--;                 // chord-forming window: no output
        else
            trig_active = rt_field ? K_DOWN : K_LEFT;
    } else {
        trig_settle = TRIG_CHORD_SETTLE;   // re-arm for the next fresh press
    }
    // A chord whose charge timed out stays silent until it changes.
    if (ch_cancel_key != 0 && trig_active != ch_cancel_key)
        ch_cancel_key = 0;
    sw_armed_key = 0;
    sw_armed_melee = false;
    sw_armed_tech_left = false;
    if (trig_active != 0) {
        // Mode 2: only the regular attack arms; heavy/special press
        // through so the charge runs. Mode 1: all chords arm. A chord
        // also presses through when its slot would not attack right now
        // (a context action or an item) - re-checked every frame, so
        // aiming at an NPC mid-hold interacts at once. Tech slots arm
        // under cast_swing_mode (any weapon, any chord).
        const bool arm_eligible =
            swing_mode && (vrmod::config.swing_attack == 1 ||
                           trig_active == K_DOWN);
        if ((arm_eligible || cast_swing_mode) &&
            PaletteArrowArms(trig_active, ctrl_swap, arm_eligible,
                             cast_swing_mode)) {
            sw_armed_key = trig_active;    // the swing executes it
            // A melee chord unless the slot holds a tech.
            sw_armed_melee =
                arm_eligible &&
                !PaletteArrowArms(trig_active, ctrl_swap, false, true);
            sw_armed_tech_left =
                !sw_armed_melee && ArrowTechLeftCasts(trig_active, ctrl_swap);
            ctx_direct_key = 0;
        } else if (ch_cancel_key != 0 && trig_active == ch_cancel_key) {
            ctx_direct_key = 0;            // cancelled charge: no press-through
        } else {
            add(trig_active);              // direct press
            if ((arm_eligible || cast_swing_mode) &&
                ctx_direct_key != trig_active) {
                BYTE t = 0, p = 0;
                PaletteEntry(gamecam::ResolveEntity(), 0, t, p);
                diag::Log("swingcontext: direct press 0x%02X "
                          "(slot0 param %d)", trig_active, p);
                ctx_direct_key = trig_active;
            }
        }
        if (trig_active == K_LEFT || trig_active == K_RIGHT)
            chord_press_qpc = tq.QuadPart;  // ChargeHold's "ours" stamp
    } else {
        ctx_direct_key = 0;
    }

    // Hotkey-bar arming (HotkeyPress): a pending hotkey is the armed key
    // while no chord is armed. It lives only while its chord is held,
    // and drops on a menu, takeover loss, the timeout or its swing.
    if (hk_armed_key != 0) {
        const double armed_s = trig_qpf > 0
            ? double(tq.QuadPart - hk_armed_qpc) / double(trig_qpf) : 0.0;
        const bool held = bindings::ActionHeld(hk_armed_action);
        if (!held || menu_mode || !gameplay_drives ||
            armed_s > vrmod::config.hotkey_arm_timeout_s) {
            diag::Log("hotkeyarm: DISARMED key 0x%02X (%s)", hk_armed_key,
                      !held ? "released" : menu_mode ? "menu"
                            : !gameplay_drives ? "takeover off" : "timeout");
            hk_armed_key = 0;
        } else if (sw_armed_key == 0) {
            sw_armed_key = hk_armed_key;
            sw_armed_tech_left = hk_armed_left;
        }
    }
    // Our charged heavy/special: nothing arms the detector in mode 2, so
    // arm it stamp-only - the stamp releases the charge and lands the
    // held strikes, and no key is pressed.
    if (sw_armed_key == 0 && swing_mode &&
        ((ch_ours && ch_was_charging) ||
         (wp_ours_paced && wp_variant != 0))) {
        sw_armed_key = SW_KEY_STAMP;
        sw_armed_melee = true;
    }
    // Swing detection and the synthesized press. A menu opening or the
    // takeover stopping drops the press (palette keys navigate menus).
    SwingDetect(left_mode);
    if (hk_armed_key != 0 && sw_fire_key == hk_armed_key &&
        sw_fire_qpc != hk_seen_fire_qpc) {
        hk_seen_fire_qpc = sw_fire_qpc;  // the swing fired it
        hk_armed_key = 0;
    }
    if (menu_mode || !gameplay_drives)
        sw_fire_frames = 0;
    // A swing that releases a held strike must not press (SwingHoldSpend).
    if (sw_fire_frames > 0 && sw_fire_key == K_DOWN && SwingHoldWaiting() &&
        SwingPending()) {
        diag::Log("swinghold: press suppressed (the %s lands the held "
                  "strike)", SwingHandName());
        sw_fire_frames = 0;
        sw_gap_frames = 2;
    }
    if (sw_fire_frames > 0) {
        sw_fire_frames--;
        if (sw_fire_key != SW_KEY_STAMP)
            add(sw_fire_key);
        if (sw_fire_key == K_LEFT || sw_fire_key == K_RIGHT)
            chord_press_qpc = tq.QuadPart;  // ChargeHold's "ours" stamp
        if (sw_fire_frames == 0)
            sw_gap_frames = 2;             // guarantee >= 1 tick released
    } else if (sw_gap_frames > 0) {
        sw_gap_frames--;
    }

    // Hold a charging heavy/special until the swing (after SwingSample,
    // so it sees this frame's speed).
    ChargeHold(gameplay_drives);
    // A swing-fired attack skips its wind-up.
    SwingWarp(gameplay_drives);


    // Sticks. The left stick always walks. The right stick turns in the
    // field, but not while a menu owns it (Left/Right Arrow there; Up/Down
    // on the dominant axis past a firm threshold) or while a hotkey-flick
    // grip is held (psobbvr_bindings.hpp).
    const float lx = DeadzoneCurve(cs.stick_x[0], dz);
    const float ly = DeadzoneCurve(cs.stick_y[0], dz);
    // With the Quick Menu up (our own latch), X stays the turn; its
    // chord pressed again cycles the tabs. Y is still Up/Down.
    const bool quick_menu = menu_mode && bindings::quick_menu_open;
    const bool turn_ok = (!menu_mode || quick_menu) &&
                         !bindings::StickIsSelector(held_mods);
    const float rx = turn_ok ? DeadzoneCurve(cs.stick_x[1], dz) : 0.0f;
    const float rxr = cs.stick_x[1];
    const float ry = cs.stick_y[1];
    if (menu_mode) {
        if (fabsf(ry) >= 0.6f && fabsf(ry) > fabsf(rxr))
            add(ry > 0 ? K_UP : K_DOWN);
        else if (!quick_menu && fabsf(rxr) >= 0.6f && fabsf(rxr) > fabsf(ry))
            add(rxr > 0 ? K_RIGHT : K_LEFT);
    }

    if (vrmod::config.stick_locomotion) {
        // Publish the raw stick state; the movement module's input
        // detour does the rest (and owns stick_turn_scale). The deadzone
        // is radial so light diagonals are not snapped to an axis.
        const float mx = cs.stick_x[0], my = cs.stick_y[0];
        const float mmag = sqrtf(mx * mx + my * my);
        float ms = 0.0f, mf = 0.0f;
        if (mmag > dz) {
            const float t =
                ((mmag > 1.0f ? 1.0f : mmag) - dz) / (1.0f - dz);
            ms = mx * (t / mmag);
            mf = my * (t / mmag);
        }
        movement::stick_raw_side = side_sign * ms;
        movement::stick_raw_fwd = -mf;
        movement::stick_raw_turn = side_sign * rx;
        movement::stick_raw_active =
            ms != 0.0f || mf != 0.0f || rx != 0.0f;
        movement::stick_move_active = false;
    } else {
        movement::stick_raw_active = false;
        // Variable-speed turn: squared curve so light deflection gives a
        // fine slow turn, full push the configured maximum.
        float turn = 0.0f;
        if (rx != 0.0f) {
            const float t = fabsf(rx);
            const float lo = vrmod::config.controller_turn_min;
            const float hi = vrmod::config.controller_turn_max;
            movement::stick_turn_scale = lo + (hi - lo) * t * t;
            turn = rx;
        } else {
            movement::stick_turn_scale = -1.0f;
        }
        // Turn mixed into the side axis (a right turn is a held D key).
        // Forward is negative (the game's convention, InputFillDetour).
        float side = side_sign * (lx + turn);
        float fwd = -ly;
        if (side > 1.0f) side = 1.0f;
        if (side < -1.0f) side = -1.0f;
        movement::stick_move_side = (int)lroundf(side * 128.0f);
        movement::stick_move_fwd = (int)lroundf(fwd * 128.0f);
        movement::stick_move_active =
            movement::stick_move_side != 0 || movement::stick_move_fwd != 0;
    }

    // Palette-map calibration: exactly one palette arrow in the field
    // with no Ctrl.
    if (gameplay_drives && !menu_mode) {
        BYTE arrow = 0;
        int arrows = 0;
        bool ctrl = false;
        for (int i = 0; i < n; i++) {
            if (keys[i] == K_DOWN || keys[i] == K_LEFT ||
                keys[i] == K_RIGHT) {
                arrow = keys[i];
                arrows++;
            } else if (keys[i] == K_LCTRL) {
                ctrl = true;
            }
        }
        if (arrows == 1 && !ctrl)
            PaletteCalibrate(arrow);
    }


    PushKeys(keys, n);
}

}  // namespace controller
