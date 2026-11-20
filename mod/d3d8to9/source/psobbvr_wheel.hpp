#pragma once

// The spell wheel's game side: the armed choice, and four detours that let
// the game cast it from a palette slot no player sees. Nothing of the
// player's palette is ever written, and no packet is built here: a cast
// goes out through the game's own executor, so the packets are the ones a
// palette press of that entry would send.
//
// The hidden slot. Every palette action reads its slot through the getters
// 0x68AA04 (slot -> type) and 0x68AA6C (slot -> param), and every palette
// loop in the game runs slots 0..13 from constants, so slot 14 is ours:
//  - The getter detours answer slot 14 on the local player with the armed
//    choice: {3, id} for a technique, {4, 13 + kind} for a trap (the
//    item-use table 0x93C5C0 sends params 13..16 to the trap setter
//    0x6959C0, trap types 0..3). Every other slot and entity runs stock;
//    the combo scanners 0x6A0B88 / 0x6A1088 ask for slot -1.
//  - Firing: the palette dispatcher 0x68A40C runs once per idle, walk and
//    run tick (0x69B728 / 0x69BBD0 / 0x6A15A0), the point where a hotkey
//    fires. After it, if it fired nothing and a fire request is pending
//    and ready, the detour runs the executor 0x68C190(entity, 14, NULL):
//    the game's own type switch, gate, latch (+0x354 / +0x355) and action
//    start (0x698A08(8, 5) for a technique, 0x68AA8C + 0x68AADC for a
//    trap). Nothing fires while [0x9D09E8], the game-actions-enabled flag,
//    is 0 (a text field is open). The HUD button flash 0x71825C is not
//    called: it takes a button number, and hotkeys skip it too.
//  - Targets: a technique casts at the output list of its targeting bank,
//    which the cast commit (0x6E1A48) finds with 0x7A401C(3, id). The roll
//    0x7A1728 builds the banks each frame from slots 0..13, compactly in
//    sort-key order, into the 14 records at tgt+0x84 + n*0x124 (tgt =
//    [entity+0x45C]); records past the last stay stale (flags bit 0, set
//    for all 14 by the roll task 0x7A1548 first). After the roll, the
//    detour writes the armed technique's bank into the highest stale
//    record with the game's bank writer 0x7A19C0, when the technique has
//    no bank yet (it is not on the bar), its sort key 0x9FF674[id] is
//    nonzero and the roll's own gate 0x69A088 passes. The candidates then
//    insert themselves (0x7A2B5C) and the finalizer 0x7A2290 commits it
//    like any bank; it comes last, so a bar bank's target wins the
//    reticle when both have one.
//  - The reticle helper 0x6ABA4C maps a committed bank's (category,
//    subcategory) to the palette slot holding it; its callers use the slot
//    only as an index into the 14-entry colour table 0x9FF540, and -1
//    kills the reticle. For the armed technique it answers 4, the colour a
//    hotkey technique shows.
//
// Two-frame rule: the press commits the cast at once from the bank's
// output list, which the finalizer fills at the end of a frame, and the
// bank writer zeroes a record's outputs whenever its category or
// subcategory change. So a technique fires only once its bank has stood
// in the same record for two rolls in a row. A technique that gets no
// bank (sort key 0, Ryuker; no free record; the gate refused it) fires
// without waiting and the game decides, as for a stock press. A trap
// needs no bank.
//
// A fire request lasts FIRE_TICKS dispatcher ticks and FIRE_FRAMES
// presented frames, whichever ends first: the dispatcher is not called
// during attacks and casts, and a request held through one would fire
// late. A palette action fired by the dispatcher in the same tick drops
// the request: a held palette button always wins.
//
// Threads: the game's mode loop (0x7A5E84, run by the thread procedure
// 0x82D2CC that 0x82C817 starts; its mode functions in the table 0x9FF7C0)
// runs the frame routine 0x80030C, which calls the task-tree
// update (0x7A6268 -> 0x817574 on the root task 0xACA2AC: the roll task,
// the entity ticks with the dispatcher, the finalizer task) and the
// device's BeginScene, EndScene and Present (0x83ADBC, 0x83ADDC,
// 0x83AE04) one after another. Tick (from Present), the controller's
// input (from BeginScene) and the detours therefore run on one thread and
// never overlap, so plain variables are enough.
//
// Every detour is inert - one or two compares around the original - while
// [wheel] enabled is 0 or nothing is armed. All four are installed even
// when the wheel is off at startup, so a live reload can turn it on.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "MinHook.h"
#include "psobbvr_gamecam.hpp"
#include "psobbvr_log.hpp"
#include "psobbvr_probe.hpp"
#include "psobbvr_wheel_config.hpp"
#include "psobbvr_wheel_core.hpp"

namespace wheel {

using wheelcore::Entry;
using wheelcore::Kind;

// ---- game routines (calling conventions read from their code) -----------

// The targeting roll: configures one player's 14 bank records from palette
// slots 0..13. thiscall on the targeting object, no stack arguments
// (plain ret); its only caller is the roll task's update 0x7A1548, after
// that has marked every record stale.
constexpr uintptr_t ROLL_ADDR = 0x007A1728;
// The bank writer: thiscall on the targeting object (entity, record,
// category, subcategory), ret 0x10. Zeroes the record's candidate count
// (+0x40) and output count (+0x9C) when the pair differs from what the
// record held (0x7A21A4), then configures it and clears its stale bit.
constexpr uintptr_t BANK_WRITE_ADDR = 0x007A19C0;
// The bank lookup: thiscall on the targeting object (category,
// subcategory), ret 8. The first of the 14 records holding the pair with
// its stale bit clear, else 0.
constexpr uintptr_t BANK_FIND_ADDR = 0x007A401C;
// The technique gate: thiscall on the entity (id), ret 4; nonzero = it
// can be cast now (learned, TP, not in a town or lobby, status, the
// Ryuker / Reverser rules). The roll gates technique slots with it; its
// one side effect is the static record buffer 0xA96650.
constexpr uintptr_t TECH_GATE_ADDR = 0x0069A088;
// The reticle helper: thiscall on palette bank 0 (entity+0x538) (entity,
// category, subcategory), ret 0xC; the slot holding the pair, 0 for the
// forced pair, -1 for none. Callers: the reticle check 0x7A32EC, the
// reticle setup 0x7A42A0, the lock-marker draw 0x7A39DC.
constexpr uintptr_t RETICLE_SLOT_ADDR = 0x006ABA4C;
// The slot getters: thiscall on the entity (slot), ret 4; the live
// palette entry's type / param byte, zero-extended. No range check.
constexpr uintptr_t SLOT_TYPE_ADDR = 0x0068AA04;
constexpr uintptr_t SLOT_PARAM_ADDR = 0x0068AA6C;
// The palette dispatcher: thiscall on the entity, no stack arguments
// (plain ret); 1 when a palette button or hotkey started an action, else
// 0 (also when a modifier key holds the hotkeys off).
constexpr uintptr_t DISPATCH_ADDR = 0x0068A40C;
// The palette executor: thiscall on the entity (slot, target), ret 8; the
// stock caller is button 1's branch of the dispatcher (0x68A475). 1 = the
// slot's action started.
constexpr uintptr_t EXECUTE_ADDR = 0x0068C190;

// ---- game data ------------------------------------------------------------

// Technique sort keys, a signed byte per id: the roll's key for a
// technique slot (0x7A1874). 0 (Ryuker) = the roll builds no bank.
constexpr uintptr_t TECH_SORT_KEY_ADDR = 0x009FF674;
// Nonzero makes the roll configure nothing (0x7A1740).
constexpr uintptr_t ROLL_BLOCK_ADDR = 0x00A48A9C;
// Game actions enabled: the edit box's open routine 0x72FDD0 writes 0
// each tick a text field is open; the action reader 0x78FD68 refuses key
// and pad actions while it is 0.
constexpr uintptr_t ACTIONS_ENABLED_ADDR = 0x009D09E8;
// The entity table has 12 entries; the resolver 0x68D5A8 returns 0 for an
// id outside 0..11.
constexpr uint32_t ENTITY_IDS = 12;

// ---- player entity --------------------------------------------------------

constexpr uintptr_t ENT_ACTION_FLAGS_OFF = 0x33C;  // dword; 0x100 = busy (0x689410)
constexpr uint32_t ENT_BUSY_BIT = 0x100;
constexpr uintptr_t ENT_TARGETING_OFF = 0x45C;     // ptr: the targeting object
constexpr uintptr_t ENT_TP_MAX_OFF = 0x2BE;        // word: maximum TP
constexpr uintptr_t ENT_TP_OFF = 0x336;            // word: current TP
// 19 bytes, one per technique id: the learned level minus one, 0xFF = not
// learned (the technique object at +0x460, its +0x48).
constexpr uintptr_t ENT_TECH_LEVELS_OFF = 0x4A8;
constexpr uint8_t TECH_NOT_LEARNED = 0xFF;
constexpr uintptr_t ENT_TRAP_COUNTS_OFF = 0x89C;   // 4 signed bytes by trap type
constexpr uintptr_t ENT_CLASS_OFF = 0x961;         // byte: class (wheelcore::kClasses)
constexpr uintptr_t ENT_RACE_FLAGS_OFF = 0x964;    // dword; 0x10 = android
constexpr uint32_t ENT_ANDROID_BIT = 0x10;
// The rest of the character file's visual record (the class byte above is
// part of it): the creation pad's proportion pair, two floats, and the
// name, 16 UTF-16 units with the language marker (a tab and a letter)
// first - the record's +0x48 and +0x50, at entity +0x930.
constexpr uintptr_t ENT_PROPORTION_OFF = 0x978;
constexpr uintptr_t ENT_NAME_OFF = 0x980;
constexpr int ENT_NAME_UNITS = 16;
constexpr uint16_t NAME_MARKER = 0x0009;           // the language marker's tab

// ---- targeting object -----------------------------------------------------

constexpr uintptr_t TGT_FLAGS_OFF = 0x1C;           // dword; bit 0 = configure nothing
constexpr uintptr_t TGT_OWNER_ID_OFF = 0x20;        // word: the owner's client id
constexpr uintptr_t TGT_BANKS_OFF = 0x84;
constexpr uintptr_t TGT_BANK_STRIDE = 0x124;
constexpr int TGT_BANK_COUNT = 14;
// Word; bit 0 set = no forced single-bank mode while the owner is busy
// (0x7A176F).
constexpr uintptr_t TGT_FORCED_FLAGS_OFF = 0x10B6;
constexpr uintptr_t BANK_CAT_OFF = 0x00;            // byte: category = palette type
constexpr uintptr_t BANK_SUB_OFF = 0x01;            // byte: subcategory = param
constexpr uintptr_t BANK_FLAGS_OFF = 0x02;          // word; bit 0 = stale
constexpr uint16_t BANK_STALE_BIT = 1;
constexpr uintptr_t BANK_OUTPUT_COUNT_OFF = 0x9C;   // dword

// ---- palette --------------------------------------------------------------

constexpr int PALETTE_SLOTS = 14;
constexpr int WHEEL_SLOT = 14;            // the hidden slot the getters answer
constexpr uint32_t TYPE_TECHNIQUE = 3;
constexpr uint32_t TYPE_ITEM = 4;
constexpr uint32_t TRAP_PARAM_BASE = 13;  // item params 13..16 = trap types 0..3
// A hotkey slot: its reticle colour (0x9FF540 + 4*0x10) is the one a bar
// technique shows.
constexpr int RETICLE_COLOUR_SLOT = 4;

constexpr int FIRE_TICKS = 8;   // dispatcher ticks a fire request waits
constexpr int FIRE_FRAMES = 8;  // ...and presented frames

// __fastcall stands in for the game's thiscall (edx unused); the stack
// arguments are callee-cleaned in both.
using RollFn = void(__fastcall*)(uintptr_t tgt, uintptr_t edx);
using BankWriteFn = void(__fastcall*)(uintptr_t tgt, uintptr_t edx, uintptr_t entity,
                                      int record, int category, int subcategory);
using BankFindFn = uintptr_t(__fastcall*)(uintptr_t tgt, uintptr_t edx, int category,
                                          int subcategory);
using GateFn = int(__fastcall*)(uintptr_t entity, uintptr_t edx, int id);
using ReticleFn = int(__fastcall*)(uintptr_t palette, uintptr_t edx, uintptr_t entity,
                                   int category, int subcategory);
using SlotGetterFn = uint32_t(__fastcall*)(uintptr_t entity, uintptr_t edx, int slot);
using DispatchFn = int(__fastcall*)(uintptr_t entity, uintptr_t edx);
using ExecuteFn = int(__fastcall*)(uintptr_t entity, uintptr_t edx, int slot, uintptr_t target);

inline RollFn roll_original = nullptr;
inline ReticleFn reticle_original = nullptr;
inline SlotGetterFn type_original = nullptr;
inline SlotGetterFn param_original = nullptr;
inline DispatchFn dispatch_original = nullptr;
inline bool roll_hooked = false;
inline bool reticle_hooked = false;
inline bool type_hooked = false;
inline bool param_hooked = false;
inline bool dispatch_hooked = false;

// ---- state ----------------------------------------------------------------

// What the last roll of the local player's targeting object found for the
// armed technique.
enum class BankState : int {
    None = 0,     // no roll since the choice changed
    NotPalette,   // the roll took an early out or its forced single-bank branch
    NoKey,        // sort key 0: the game builds no bank for it either
    OnBar,        // the bar built its bank
    GateRefused,  // the gate refused it (not learned, TP, town, status)
    NoRoom,       // every record holds a bar bank: it casts unaimed
    Written,      // the wheel wrote its bank
};

inline const char* BankStateName(BankState s) {
    switch (s) {
    case BankState::NotPalette: return "roll not configuring the palette";
    case BankState::NoKey: return "no bank (sort key 0)";
    case BankState::OnBar: return "bank built by the bar";
    case BankState::GateRefused: return "gate refused";
    case BankState::NoRoom: return "no free record";
    case BankState::Written: return "bank written";
    default: return "no roll yet";
    }
}

struct RollInfo {
    BankState state = BankState::None;
    int record = -1;      // the bank's record (Written / OnBar), else -1
    int streak = 0;       // consecutive rolls with the bank in that record
    unsigned frame = 0;   // frame_serial at the roll
};

inline Entry choice;                     // Kind::None = nothing armed
inline unsigned frame_serial = 0;        // presented frames (Tick)
inline unsigned frames_since_change = 0; // since the choice last changed
inline RollInfo roll;
inline bool wrote_this_frame = false;    // the roll detour wrote the bank
inline int wrote_record = -1;
inline bool wrote_last_frame = false;    // ...in the frame Tick last closed
inline int wrote_last_record = -1;
inline bool no_room_logged = false;      // once per choice

inline bool fire_pending = false;
inline int fire_ticks_left = 0;
inline int fire_frames_left = 0;
inline int fire_ticks_waited = 0;

inline bool Armed() { return choice.kind != Kind::None; }
inline Entry Choice() { return choice; }

// The palette entry {type, param} an entry stands for.
inline uint32_t EntryType(Entry e) {
    return e.kind == Kind::Technique ? TYPE_TECHNIQUE : e.kind == Kind::Trap ? TYPE_ITEM : 0;
}
inline uint32_t EntryParam(Entry e) {
    return e.kind == Kind::Technique ? (uint32_t)e.index
         : e.kind == Kind::Trap ? TRAP_PARAM_BASE + (uint32_t)e.index : 0;
}

// The local player's entity as the game resolves it (0x68D5A8 with the
// local id at gamecam::ENTITY_INDEX_ADDR); both reads are of the exe's own
// data section. Cheap enough for the per-call checks below.
inline uintptr_t LocalEntity() {
    const uint32_t id = *reinterpret_cast<const volatile uint32_t*>(gamecam::ENTITY_INDEX_ADDR);
    if (id >= ENTITY_IDS)
        return 0;
    return *reinterpret_cast<const volatile uintptr_t*>(gamecam::ENTITY_TABLE_ADDR + 4 * id);
}

// The entity for a client id, as 0x68D5A8 returns it.
inline uintptr_t EntityForId(uint32_t id) {
    if (id == 0xFFFF || id >= ENTITY_IDS)
        return 0;
    return *reinterpret_cast<const uintptr_t*>(gamecam::ENTITY_TABLE_ADDR + 4 * id);
}

inline uintptr_t BankAt(uintptr_t tgt, int record) {
    return tgt + TGT_BANKS_OFF + (uintptr_t)record * TGT_BANK_STRIDE;
}
inline int RecordOf(uintptr_t tgt, uintptr_t bank) {
    if (bank < tgt + TGT_BANKS_OFF)
        return -1;
    const uintptr_t off = bank - (tgt + TGT_BANKS_OFF);
    const uintptr_t n = off / TGT_BANK_STRIDE;
    return off % TGT_BANK_STRIDE == 0 && n < (uintptr_t)TGT_BANK_COUNT ? (int)n : -1;
}
inline bool RecordStale(uintptr_t tgt, int record) {
    return (*reinterpret_cast<const uint16_t*>(BankAt(tgt, record) + BANK_FLAGS_OFF) &
            BANK_STALE_BIT) != 0;
}

inline int TechSortKey(int id) {
    return *reinterpret_cast<const int8_t*>(TECH_SORT_KEY_ADDR + (uintptr_t)id);
}

inline bool ActionsEnabled() {
    return *reinterpret_cast<const volatile uint32_t*>(ACTIONS_ENABLED_ADDR) != 0;
}

// ---- castability ----------------------------------------------------------

inline bool castable_cached = false;
inline unsigned castable_frame = 0;      // frame_serial of the answer
inline Entry castable_choice;
inline uintptr_t castable_entity = 0;
inline bool castable_value = false;

// Would the game take the armed choice now? A technique: learned, and the
// gate passes (TP, not in a town or lobby, status). A trap: an android
// with one left. Nothing armed or no local player = no. The controller
// asks twice per presented frame, so the answer is kept for the frame
// (frame_serial), the choice and the entity; the gate's only side effect
// is the static record buffer, and the roll calls it every frame anyway.
// The game still decides at the fire: TP can drop in between.
inline bool ChoiceCastableNow() {
    if (!Armed())
        return false;
    const uintptr_t e = LocalEntity();
    if (castable_cached && castable_frame == frame_serial && castable_choice == choice &&
        castable_entity == e)
        return castable_value;
    bool ok = false;
    // Every field read here lies below the race flags.
    if (e != 0 && diag::Accessible(e, ENT_RACE_FLAGS_OFF + 4, false)) {
        if (choice.kind == Kind::Technique) {
            const uint8_t lv = *reinterpret_cast<const uint8_t*>(
                e + ENT_TECH_LEVELS_OFF + (uintptr_t)choice.index);
            ok = lv != TECH_NOT_LEARNED &&
                 reinterpret_cast<GateFn>(TECH_GATE_ADDR)(e, 0, choice.index) != 0;
        } else if (choice.kind == Kind::Trap) {
            const bool android =
                (*reinterpret_cast<const uint32_t*>(e + ENT_RACE_FLAGS_OFF) & ENT_ANDROID_BIT) != 0;
            ok = android && *reinterpret_cast<const int8_t*>(
                                e + ENT_TRAP_COUNTS_OFF + (uintptr_t)choice.index) > 0;
        }
    }
    castable_cached = true;
    castable_frame = frame_serial;
    castable_choice = choice;
    castable_entity = e;
    castable_value = ok;
    return ok;
}

// ---- the choice and the fire request --------------------------------------

inline void DropFire(const char* why) {
    if (!fire_pending)
        return;
    fire_pending = false;
    diag::Log("wheel: fire request dropped (%s; %s)", wheelcore::KeyOf(choice), why);
}

inline void ExpireFire(const char* why) {
    fire_pending = false;
    diag::Log("wheel: fire request expired after %d tick(s) (%s; %s)", fire_ticks_waited,
              wheelcore::KeyOf(choice), why);
}

inline void ResetForChange() {
    frames_since_change = 0;
    roll = RollInfo();
    no_room_logged = false;
}

inline void Clear() {
    if (!Armed())
        return;
    DropFire("the choice was cleared");
    diag::Log("wheel: cleared (was %s)", wheelcore::KeyOf(choice));
    choice = Entry();
    ResetForChange();
}

// Arm an entry; its index is clamped (an id outside the game's table
// passes the learned test and casts as Foie). An empty entry clears.
inline void Arm(Entry e) {
    e = wheelcore::Clamped(e);
    if (e.kind == Kind::None) {
        Clear();
        return;
    }
    if (e == choice)
        return;
    DropFire("the choice changed");
    choice = e;
    ResetForChange();
    diag::Log("wheel: armed %s (%s; hidden slot %d reads {%u, %u})", wheelcore::KeyOf(e),
              wheelcore::NameOf(e), WHEEL_SLOT, EntryType(e), EntryParam(e));
}

// ---- the character the choice belongs to ----------------------------------

// The choice lives for the whole process, so it is cleared when a different
// character appears: a new character starts with nothing armed. The key is
// the character's own data - class byte, proportion pair and name - not the
// entity's address, which the game hands to a new character on some
// relogs. Joining a game, an area change, a teleport, a death and the
// return to town keep the same data, so none of them clears. A character
// entered again unchanged keeps its choice: its data cannot tell that
// apart from an area change.
struct Identity {
    uint8_t cls = 0;
    float pair[2] = {};
    uint16_t name[ENT_NAME_UNITS] = {};  // up to the terminator, zero after
};

inline bool SameIdentity(const Identity& a, const Identity& b) {
    return a.cls == b.cls && memcmp(a.pair, b.pair, sizeof(a.pair)) == 0 &&
           memcmp(a.name, b.name, sizeof(a.name)) == 0;
}

// The character in an entity; false when the record is unreadable or does
// not hold one (a class out of range, a name without its marker).
inline bool ReadIdentity(uintptr_t e, Identity& out) {
    if (e == 0 || !diag::Accessible(e + ENT_CLASS_OFF,
                                    ENT_NAME_OFF + sizeof(out.name) - ENT_CLASS_OFF, false))
        return false;
    Identity id;
    id.cls = *reinterpret_cast<const uint8_t*>(e + ENT_CLASS_OFF);
    memcpy(id.pair, reinterpret_cast<const void*>(e + ENT_PROPORTION_OFF), sizeof(id.pair));
    const uint16_t* name = reinterpret_cast<const uint16_t*>(e + ENT_NAME_OFF);
    for (int i = 0; i < ENT_NAME_UNITS && name[i] != 0; i++)
        id.name[i] = name[i];
    if (id.cls >= wheelcore::kClassCount || id.name[0] != NAME_MARKER)
        return false;
    out = id;
    return true;
}

inline bool identity_valid = false;    // a character seen since launch
inline Identity identity;              // the last one seen
inline unsigned character_serial = 0;  // +1 at each change (the input side closes the ring)

// From Tick. Only frames the camera takeover drives are read: their entity
// is the one it resolved this frame (a live camera, sane feet), and the
// wheel does nothing on any other frame. With no character in reach the
// last one stands, so the screens between two characters never clear
// anything; only a different one does.
inline void WatchCharacter() {
    if (!gamecam::DrivesView())
        return;
    Identity now;
    if (!ReadIdentity(gamecam::ResolveEntity(), now))
        return;
    if (identity_valid && !SameIdentity(now, identity)) {
        character_serial++;
        diag::Log("wheel: character changed (class %s -> %s) - %s",
                  wheelcore::kClasses[identity.cls].key, wheelcore::kClasses[now.cls].key,
                  Armed() ? "choice cleared" : "nothing was armed");
        Clear();
    }
    identity = now;
    identity_valid = true;
}

// A roll this frame or last (the per-frame order of the roll and Tick
// does not matter then).
inline bool RollRecent(unsigned frame) { return frame + 1u >= frame_serial; }

// Armed, and for a technique its bank has stood with outputs for a full
// frame (or it cannot have one); a trap as soon as it is armed.
inline bool FireReady() {
    if (!wheelconfig::config.enabled || !Armed())
        return false;
    // Without both getters slot 14 would read past the palette.
    if (!type_hooked || !param_hooked)
        return false;
    if (choice.kind == Kind::Trap)
        return true;
    if (!roll_hooked)
        return true;  // no bank can be added: the bar's bank or none
    if (roll.state == BankState::None || !RollRecent(roll.frame))
        return false;
    switch (roll.state) {
    case BankState::Written:
    case BankState::OnBar:
        return roll.streak >= 2;
    case BankState::NoKey:
    case BankState::NoRoom:
    case BankState::GateRefused:
        return true;
    default:
        return false;
    }
}

// Post a fire request; the dispatcher detour serves it on the game's next
// idle, walk or run tick once FireReady holds.
inline void RequestFire() {
    if (!Armed()) {
        diag::Log("wheel: fire request ignored - nothing armed");
        return;
    }
    if (!wheelconfig::config.enabled) {
        diag::Log("wheel: fire request ignored - [wheel] enabled is 0");
        return;
    }
    if (!dispatch_hooked || !type_hooked || !param_hooked) {
        diag::Log("wheel: fire request ignored - the dispatcher or getter hooks are not "
                  "installed");
        return;
    }
    fire_pending = true;
    fire_ticks_left = FIRE_TICKS;
    fire_frames_left = FIRE_FRAMES;
    fire_ticks_waited = 0;
    diag::Log("wheel: fire requested (%s%s)", wheelcore::KeyOf(choice),
              FireReady() ? "" : "; waiting for its bank");
}

// Once per presented frame, from Present (BeginScene, where the
// controller runs, comes twice per frame).
inline void Tick() {
    frame_serial++;
    if (frames_since_change < 0x7FFFFFFFu)
        frames_since_change++;
    wrote_last_frame = wrote_this_frame;
    wrote_last_record = wrote_record;
    wrote_this_frame = false;
    wrote_record = -1;
    if (fire_pending && --fire_frames_left <= 0)
        ExpireFire(FireReady() ? "no idle, walk or run tick took it" : "its bank was not ready");
    WatchCharacter();
}

// ---- detours ----------------------------------------------------------------

// The roll's own early outs (0x7A1731..0x7A1779): it configures the
// palette's banks only when these pass.
inline bool RollTakesPalette(uintptr_t tgt, uintptr_t owner) {
    if ((*reinterpret_cast<const uint32_t*>(tgt + TGT_FLAGS_OFF) & 1u) != 0)
        return false;
    if (*reinterpret_cast<const uint32_t*>(ROLL_BLOCK_ADDR) != 0)
        return false;
    if (owner == 0)
        return false;
    const bool busy =
        (*reinterpret_cast<const uint32_t*>(owner + ENT_ACTION_FLAGS_OFF) & ENT_BUSY_BIT) != 0;
    return !busy || (*reinterpret_cast<const uint16_t*>(tgt + TGT_FORCED_FLAGS_OFF) & 1u) != 0;
}

// After the local player's roll: give the armed technique its bank.
inline void AppendBank(uintptr_t tgt, uintptr_t entity, bool palette) {
    RollInfo r;
    r.frame = frame_serial;
    const int id = choice.index;
    if (!palette) {
        r.state = BankState::NotPalette;
    } else if (TechSortKey(id) == 0) {
        r.state = BankState::NoKey;
    } else if (const uintptr_t bar =
                   reinterpret_cast<BankFindFn>(BANK_FIND_ADDR)(tgt, 0, (int)TYPE_TECHNIQUE, id)) {
        r.state = BankState::OnBar;
        r.record = RecordOf(tgt, bar);
    } else if (reinterpret_cast<GateFn>(TECH_GATE_ADDR)(entity, 0, id) == 0) {
        r.state = BankState::GateRefused;
    } else {
        int record = -1;
        for (int i = TGT_BANK_COUNT - 1; i >= 0 && record < 0; i--)
            if (RecordStale(tgt, i))
                record = i;
        if (record < 0) {
            r.state = BankState::NoRoom;
            if (!no_room_logged) {
                no_room_logged = true;
                diag::Log("wheel: no free bank record for %s (the bar uses all 14) - it "
                          "casts unaimed", wheelcore::KeyOf(choice));
            }
        } else {
            reinterpret_cast<BankWriteFn>(BANK_WRITE_ADDR)(tgt, 0, entity, record,
                                                           (int)TYPE_TECHNIQUE, id);
            r.state = BankState::Written;
            r.record = record;
            wrote_this_frame = true;
            wrote_record = record;
        }
    }
    // The streak survives only an unbroken run: a record left stale for a
    // while keeps its old outputs, and rewriting the same pair does not
    // zero them.
    if (r.record >= 0)
        r.streak = r.state == roll.state && r.record == roll.record && RollRecent(roll.frame)
                       ? roll.streak + 1
                       : 1;
    roll = r;
}

inline void __fastcall RollDetour(uintptr_t tgt, uintptr_t edx) {
    if (!wheelconfig::config.enabled || choice.kind != Kind::Technique) {
        roll_original(tgt, edx);
        return;
    }
    const uintptr_t local = LocalEntity();
    const bool mine = local != 0 && tgt != 0 &&
                      *reinterpret_cast<const uintptr_t*>(local + ENT_TARGETING_OFF) == tgt;
    // Decided before the call, from what the original tests at its start.
    bool palette = false;
    if (mine) {
        const uintptr_t owner =
            EntityForId(*reinterpret_cast<const uint16_t*>(tgt + TGT_OWNER_ID_OFF));
        palette = owner == local && RollTakesPalette(tgt, owner);
    }
    roll_original(tgt, edx);
    if (mine)
        AppendBank(tgt, local, palette);
}

inline int __fastcall ReticleDetour(uintptr_t palette, uintptr_t edx, uintptr_t entity,
                                    int category, int subcategory) {
    const int slot = reticle_original(palette, edx, entity, category, subcategory);
    if (slot != -1 || choice.kind != Kind::Technique || !wheelconfig::config.enabled)
        return slot;
    // The game reads both as the low byte of a word.
    if ((uint32_t)(category & 0xFF) != TYPE_TECHNIQUE || (subcategory & 0xFF) != choice.index ||
        entity == 0 || entity != LocalEntity())
        return slot;
    return RETICLE_COLOUR_SLOT;
}

// Slot 14 on the local player while armed; everything else is stock.
inline bool AnswersSlot(uintptr_t entity) {
    return Armed() && wheelconfig::config.enabled && entity != 0 && entity == LocalEntity();
}

inline uint32_t __fastcall TypeDetour(uintptr_t entity, uintptr_t edx, int slot) {
    if (slot != WHEEL_SLOT || !AnswersSlot(entity))
        return type_original(entity, edx, slot);
    return EntryType(choice);
}

inline uint32_t __fastcall ParamDetour(uintptr_t entity, uintptr_t edx, int slot) {
    if (slot != WHEEL_SLOT || !AnswersSlot(entity))
        return param_original(entity, edx, slot);
    return EntryParam(choice);
}

inline int __fastcall DispatchDetour(uintptr_t entity, uintptr_t edx) {
    const int fired = dispatch_original(entity, edx);
    if (!fire_pending || entity == 0 || entity != LocalEntity())
        return fired;
    if (!wheelconfig::config.enabled || !Armed()) {
        DropFire("the wheel is off");
        return fired;
    }
    if (fired != 0) {
        DropFire("a palette action fired this tick");
        return fired;
    }
    fire_ticks_waited++;
    const bool ready = FireReady();
    const bool actions = ActionsEnabled();
    if (ready && actions) {
        fire_pending = false;
        const int started = reinterpret_cast<ExecuteFn>(EXECUTE_ADDR)(entity, 0, WHEEL_SLOT, 0);
        diag::Log("wheel: fire served (%s, after %d tick(s)): the executor returned %d%s",
                  wheelcore::KeyOf(choice), fire_ticks_waited, started,
                  started ? "" : " - the game refused it");
        return started;
    }
    if (--fire_ticks_left <= 0)
        ExpireFire(!actions ? "game actions disabled (a text field is open)"
                            : "its bank was not ready");
    return fired;
}

// ---- install --------------------------------------------------------------

inline bool Hook(uintptr_t addr, void* detour, void** original, const char* name) {
    if (MH_CreateHook(reinterpret_cast<void*>(addr), detour, original) != MH_OK ||
        MH_EnableHook(reinterpret_cast<void*>(addr)) != MH_OK) {
        probe::Log("wheel: %s hook failed (0x%08X)", name, (unsigned)addr);
        diag::Log("wheel: %s hook FAILED (0x%08X)", name, (unsigned)addr);
        *original = nullptr;
        return false;
    }
    return true;
}

// Called from the first BeginScene, beside vrkeyboard::Install (MinHook is
// initialized by then). Always installed; gated per call.
inline void Install() {
    static bool tried = false;
    if (tried)
        return;
    tried = true;
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        diag::Log("wheel: MH_Initialize failed (%d) - no spell wheel", (int)status);
        return;
    }
    roll_hooked = Hook(ROLL_ADDR, reinterpret_cast<void*>(&RollDetour),
                       reinterpret_cast<void**>(&roll_original), "targeting-roll");
    reticle_hooked = Hook(RETICLE_SLOT_ADDR, reinterpret_cast<void*>(&ReticleDetour),
                          reinterpret_cast<void**>(&reticle_original), "reticle-slot");
    type_hooked = Hook(SLOT_TYPE_ADDR, reinterpret_cast<void*>(&TypeDetour),
                       reinterpret_cast<void**>(&type_original), "slot-type getter");
    param_hooked = Hook(SLOT_PARAM_ADDR, reinterpret_cast<void*>(&ParamDetour),
                        reinterpret_cast<void**>(&param_original), "slot-param getter");
    dispatch_hooked = Hook(DISPATCH_ADDR, reinterpret_cast<void*>(&DispatchDetour),
                           reinterpret_cast<void**>(&dispatch_original), "palette-dispatcher");
    diag::Log("wheel: hooks: roll %s, reticle %s, type getter %s, param getter %s, "
              "dispatcher %s ([wheel] enabled=%d)",
              roll_hooked ? "ok" : "FAILED", reticle_hooked ? "ok" : "FAILED",
              type_hooked ? "ok" : "FAILED", param_hooked ? "ok" : "FAILED",
              dispatch_hooked ? "ok" : "FAILED", wheelconfig::config.enabled ? 1 : 0);
}


}  // namespace wheel
