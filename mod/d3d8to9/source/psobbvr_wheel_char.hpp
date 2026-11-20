#pragma once

// The spell wheel's characters: which one is in play, its wheel, and the
// file the Options program lists the characters from.
//
// A wheel belongs to a character: the account's guild card number and the
// save slot (wheelcore::CharLayout). Both are read from the game once a
// character is in the world. The Options program cannot see the game's
// memory, so each time a character enters the world the mod writes what the
// game knows about the account's characters - slot, name, class, level -
// into psobbvr-characters.ini beside psobbvr.ini (not into psobbvr.ini: that
// is the player's file, and a write to it reloads the settings).

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "psobbvr_gamecam.hpp"
#include "psobbvr_log.hpp"
#include "psobbvr_wheel.hpp"
#include "psobbvr_wheel_config.hpp"
#include "psobbvr_wheel_core.hpp"

namespace wheelchar {

// The save slot in play (dword, from 0). Written by the slot setter
// 0x6C1A80 when a character is chosen and restored from the kept login
// command's slot byte on the lobby connection (0x7F2653). On the select
// screen it follows the cursor, so it is read only with a character in the
// world.
constexpr uintptr_t CURRENT_SLOT_ADDR = 0x00A95E48;
// The account's guild card number (dword) in the login command the client
// keeps (its body starts at 0xA46B88).
constexpr uintptr_t LOGIN_GUILD_CARD_ADDR = 0x00A46B8C;
// The character list the select screen fills from the server's previews
// (pointer); it stays allocated in the world. Slot i's record is at
// LIST_FIRST_OFF + i * LIST_STRIDE (the slot setter's own arithmetic). The
// preview reader 0x6C44D0 takes from a record the level less one (dword),
// the guild card number as text ("%10d"), the class byte and the name
// (UTF-16, the language marker first).
constexpr uintptr_t CHAR_LIST_PTR_ADDR = 0x00A95E44;
constexpr uintptr_t LIST_FIRST_OFF = 4;
constexpr uintptr_t LIST_STRIDE = 0x2EA4;
constexpr uintptr_t REC_LEVEL_OFF = 0x364;
constexpr uintptr_t REC_GUILD_CARD_OFF = 0x370;
constexpr int REC_GUILD_CARD_CHARS = 16;
constexpr uintptr_t REC_CLASS_OFF = 0x3A1;
constexpr uintptr_t REC_NAME_OFF = 0x3C0;
// The slots of the stock select screen. A server patch can add more; their
// records are not assumed to follow the same layout, so only the stock ones
// (each checked) and the character in play are listed.
constexpr int STOCK_SLOTS = 4;

struct Who {
    unsigned guild_card = 0;
    int slot = -1;
    bool valid = false;
};

// The character in play. Meaningful only with a character in the world.
inline Who Current() {
    Who w;
    w.guild_card = *reinterpret_cast<const volatile uint32_t*>(LOGIN_GUILD_CARD_ADDR);
    const uint32_t slot = *reinterpret_cast<const volatile uint32_t*>(CURRENT_SLOT_ADDR);
    w.slot = slot < (uint32_t)wheelcore::kMaxSlots ? (int)slot : -1;
    w.valid = w.guild_card != 0 && w.slot >= 0;
    return w;
}

// The wheel of the character in play, whose class byte is `cls`; null when
// it has none. `why` (optional) receives the reason in words.
inline const wheelcore::Layout* LayoutFor(int cls, const char** why = nullptr) {
    const char* dummy;
    const char*& reason = why != nullptr ? *why : dummy;
    reason = "";
    const Who w = Current();
    if (!w.valid) {
        reason = "the game does not say which character this is";
        return nullptr;
    }
    const wheelcore::CharLayout* c =
        wheelcore::FindChar(wheelconfig::config.layouts, w.guild_card, w.slot);
    if (c == nullptr || c->layout.pages <= 0) {
        reason = "nothing set up for this character";
        return nullptr;
    }
    if (c->cls >= 0 && c->cls != cls) {
        reason = "the wheel saved for this slot is another class's";
        return nullptr;
    }
    return &c->layout;
}

// Each character's page, this session.
struct PageMemo {
    unsigned guild_card;
    int slot;
    int page;
};
inline std::vector<PageMemo> page_memo;

inline int LastPage() {
    const Who w = Current();
    for (const PageMemo& m : page_memo)
        if (w.valid && m.guild_card == w.guild_card && m.slot == w.slot)
            return m.page;
    return 0;
}

inline void SetLastPage(int page) {
    const Who w = Current();
    if (!w.valid)
        return;
    for (PageMemo& m : page_memo)
        if (m.guild_card == w.guild_card && m.slot == w.slot) {
            m.page = page;
            return;
        }
    page_memo.push_back(PageMemo{w.guild_card, w.slot, page});
}

// ---- the characters file ---------------------------------------------------

// A name's units past the language marker (a tab and a letter), as UTF-8.
inline std::string NameUtf8(const uint16_t* units, int count) {
    wchar_t w[32];
    int n = 0;
    for (int i = 2; i < count && i < 32 && units[i] != 0; i++)
        w[n++] = units[i] < 0x20 ? L' ' : (wchar_t)units[i];
    char out[128];
    const int len = n > 0 ? WideCharToMultiByte(CP_UTF8, 0, w, n, out, sizeof(out), nullptr, nullptr)
                          : 0;
    return std::string(out, len > 0 ? (size_t)len : 0);
}

struct Record {
    std::string name;   // UTF-8
    int cls = -1;
    int level = 0;      // as shown; 0 unknown
    unsigned card = 0;  // the record's guild card text as a number
};

// Slot `slot`'s record in the character list; false unless it is readable
// and holds a character (its guild card text is a number, its class is
// one, its name carries the marker). The guild card text is the number the
// character was made under, which is not always the account's: a server
// can hand a character to another account as it is.
inline bool ReadRecord(int slot, Record& out) {
    const uintptr_t list = *reinterpret_cast<const volatile uintptr_t*>(CHAR_LIST_PTR_ADDR);
    if (list == 0 || slot < 0)
        return false;
    const uintptr_t rec = list + LIST_FIRST_OFF + (uintptr_t)slot * LIST_STRIDE;
    const size_t span = REC_NAME_OFF + wheel::ENT_NAME_UNITS * sizeof(uint16_t) - REC_LEVEL_OFF;
    if (!diag::Accessible(rec + REC_LEVEL_OFF, span, false))
        return false;
    char card[REC_GUILD_CARD_CHARS + 1] = {};
    memcpy(card, reinterpret_cast<const void*>(rec + REC_GUILD_CARD_OFF), REC_GUILD_CARD_CHARS);
    char* end = nullptr;
    const unsigned long number = strtoul(card, &end, 10);  // skips the padding spaces
    if (end == card || *end != '\0' || number == 0 || number > 0xFFFFFFFFul)
        return false;
    out.card = (unsigned)number;
    const int cls = *reinterpret_cast<const uint8_t*>(rec + REC_CLASS_OFF);
    uint16_t name[wheel::ENT_NAME_UNITS];
    memcpy(name, reinterpret_cast<const void*>(rec + REC_NAME_OFF), sizeof(name));
    if (cls >= wheelcore::kClassCount || name[0] != wheel::NAME_MARKER)
        return false;
    out.name = NameUtf8(name, wheel::ENT_NAME_UNITS);
    out.cls = cls;
    const uint32_t level = *reinterpret_cast<const uint32_t*>(rec + REC_LEVEL_OFF) + 1u;
    out.level = level >= 1 && level <= 200 ? (int)level : 0;
    return !out.name.empty();
}

inline bool write_failed_logged = false;

// One value of the characters file, written only when it differs. True
// when the file changed.
inline bool Put(const char* path, const char* section, const char* key, const std::string& value) {
    char cur[256];
    GetPrivateProfileStringA(section, key, "\x01", cur, sizeof(cur), path);
    if (value == cur)
        return false;
    if (!WritePrivateProfileStringA(section, key, value.c_str(), path)) {
        if (!write_failed_logged)
            diag::Log("wheel: could not write %s (error %lu) - the Options program will not "
                      "list this character", path, GetLastError());
        write_failed_logged = true;
        return false;
    }
    return true;
}

// The characters file: the character in play from its own entity (always
// right), and the stock slots' other characters from the list. The level
// comes from the list, when its record is the same character. Another
// slot's record is taken as this account's when its guild card text is the
// account's number or the same as the text in the record of the slot in
// play; anything else may be left over from an earlier login.
inline void WriteRoster(const Who& who, const wheel::Identity& id) {
    if (wheelconfig::ini_path[0] == '\0')
        return;
    char path[MAX_PATH + 32];
    snprintf(path, sizeof(path), "%s", wheelconfig::ini_path);
    char* slash = strrchr(path, '\\');
    char* name_at = slash != nullptr ? slash + 1 : path;
    snprintf(name_at, sizeof(path) - (size_t)(name_at - path), "%s", wheelcore::kRosterFile);

    Record in_play;
    const bool have_in_play = ReadRecord(who.slot, in_play);
    const int last = who.slot >= STOCK_SLOTS ? who.slot : STOCK_SLOTS - 1;
    int listed = 0, changed = 0;
    for (int slot = 0; slot <= last; slot++) {
        if (slot >= STOCK_SLOTS && slot != who.slot)
            continue;
        Record r;
        const bool have = ReadRecord(slot, r) &&
                          (slot == who.slot || r.card == who.guild_card ||
                           (have_in_play && r.card == in_play.card));
        if (slot == who.slot) {
            const std::string name = NameUtf8(id.name, wheel::ENT_NAME_UNITS);
            if (!have || r.name != name || r.cls != id.cls)
                r.level = 0;  // the list's record is not this character
            r.name = name;
            r.cls = id.cls;
        } else if (!have) {
            continue;
        }
        const std::string section = wheelcore::CharPrefix(who.guild_card, slot);
        changed += Put(path, section.c_str(), wheelcore::kRosterName, r.name) ? 1 : 0;
        changed += Put(path, section.c_str(), wheelcore::kRosterClass,
                       wheelcore::kClasses[r.cls].key) ? 1 : 0;
        if (r.level > 0)
            changed += Put(path, section.c_str(), wheelcore::kRosterLevel,
                           std::to_string(r.level)) ? 1 : 0;
        listed++;
    }
    diag::Log("wheel: characters file: guild card %u, slot %d in play (%s); %d character(s) "
              "listed, %d value(s) changed", who.guild_card, who.slot + 1,
              wheelcore::kClasses[id.cls].key, listed, changed);
}

inline Who roster_who;
inline wheel::Identity roster_identity;

// Each frame: writes the characters file when a character not yet written
// this session is in the world. Read on the frames the camera takeover
// drives only, as the character watch is: their entity is the one in play.
inline void Update() {
    if (!gamecam::DrivesView())
        return;
    wheel::Identity id;
    if (!wheel::ReadIdentity(gamecam::ResolveEntity(), id))
        return;
    const Who w = Current();
    if (!w.valid)
        return;
    if (roster_who.valid && roster_who.guild_card == w.guild_card && roster_who.slot == w.slot &&
        roster_identity.cls == id.cls &&
        memcmp(roster_identity.name, id.name, sizeof(id.name)) == 0)
        return;
    roster_who = w;
    roster_identity = id;
    WriteRoster(w, id);
}

}  // namespace wheelchar
