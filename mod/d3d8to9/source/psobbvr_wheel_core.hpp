#pragma once

// The spell wheel's content, shared by the game DLL and the Options
// program: the techniques and traps a segment can hold, the character
// classes and what each can learn, a character's pages, the [wheel]
// section's page lines, and the "fill with what this class can learn"
// helper. Standard library only; touches neither the game nor Windows.
//
// Each character has its own wheel. A character is the account's guild
// card number and the save slot it sits in; its lines carry both as their
// key's prefix, gc<number>_slot<N> (N from 1, as the select screen counts).
//
// Ini grammar ([wheel] section):
//   gc<G>_slot<N>.name  = text          the character's name, for people
//                                       reading the file (UTF-8)
//   gc<G>_slot<N>.class = <class key>   the class the pages were made for
//                                       (humar ... ramarl); the game leaves
//                                       the wheel empty for a character of
//                                       another class, as after the slot was
//                                       deleted and used again
//   gc<G>_slot<N>.pages = P             how many pages the wheel has (0..8)
//   gc<G>_slot<N>.pageK = e1, ..., e8   page K (1..8): its eight segments in
//                                       compass order N, NE, E, SE, S, SW, W,
//                                       NW
// An entry is a technique key (foie ... megid), a trap key (trap_damage,
// trap_freeze, trap_slow, trap_confuse) or "empty"; keys and entries are
// case-insensitive. A page with fewer than eight entries leaves the rest
// empty. Each of these is dropped with a warning: entries past the eighth,
// lines with an unknown prefix or setting, and unknown entries (the segment
// stays empty). The pages line lets a player's file remove pages that the
// defaults file would otherwise add back; pageK lines past it are ignored.
// Without one, the character has as many pages as it has pageK lines, in K
// order. A character with no lines has no pages: there are no default
// layouts. Lines keyed by a class (humar.page1) are ignored with one warning.

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <vector>

namespace wheelcore {

struct Named {
    const char* key;   // ini key, lowercase
    const char* name;  // English display name
};

// Techniques in the game's id order: the id is a palette entry's parameter
// (type 3) and the index of the character's learned-level byte.
constexpr int kTechCount = 19;
constexpr Named kTechs[kTechCount] = {
    {"foie", "Foie"},       {"gifoie", "Gifoie"},   {"rafoie", "Rafoie"},
    {"barta", "Barta"},     {"gibarta", "Gibarta"}, {"rabarta", "Rabarta"},
    {"zonde", "Zonde"},     {"gizonde", "Gizonde"}, {"razonde", "Razonde"},
    {"grants", "Grants"},   {"deband", "Deband"},   {"jellen", "Jellen"},
    {"zalure", "Zalure"},   {"shifta", "Shifta"},   {"ryuker", "Ryuker"},
    {"resta", "Resta"},     {"anti", "Anti"},       {"reverser", "Reverser"},
    {"megid", "Megid"},
};

// Traps in the game's trap-type order: palette entries of type 4 with
// parameters 13..16 set trap types 0..3 (the item-use table 0x93C5C0).
constexpr int kTrapCount = 4;
constexpr Named kTraps[kTrapCount] = {
    {"trap_damage", "Damage Trap"},
    {"trap_freeze", "Freeze Trap"},
    {"trap_slow", "Slow Trap"},
    {"trap_confuse", "Confuse Trap"},
};

constexpr const char* kEmptyKey = "empty";
constexpr const char* kEmptyName = "(empty)";

// Character classes by the game's class byte (entity +0x961).
constexpr int kClassCount = 12;
constexpr Named kClasses[kClassCount] = {
    {"humar", "HUmar"},       {"hunewearl", "HUnewearl"}, {"hucast", "HUcast"},
    {"ramar", "RAmar"},       {"racast", "RAcast"},       {"racaseal", "RAcaseal"},
    {"fomarl", "FOmarl"},     {"fonewm", "FOnewm"},       {"fonewearl", "FOnewearl"},
    {"hucaseal", "HUcaseal"}, {"fomar", "FOmar"},         {"ramarl", "RAmarl"},
};

// ---------------------------------------------------------------- entries

enum class Kind : int { None = 0, Technique = 1, Trap = 2 };

// One segment's content. index = technique id (0..18) or trap kind (0..3);
// 0 when empty.
struct Entry {
    Kind kind = Kind::None;
    int index = 0;
};

inline bool operator==(const Entry& a, const Entry& b) {
    return a.kind == b.kind && a.index == b.index;
}
inline bool operator!=(const Entry& a, const Entry& b) { return !(a == b); }

inline int ClampInt(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

inline Entry MakeTechnique(int id) {
    return Entry{Kind::Technique, ClampInt(id, 0, kTechCount - 1)};
}
inline Entry MakeTrap(int kind) { return Entry{Kind::Trap, ClampInt(kind, 0, kTrapCount - 1)}; }

// The entry with its index forced into range: a technique id outside the
// game's table passes its learned test and casts as Foie. An unknown kind
// is empty.
inline Entry Clamped(Entry e) {
    switch (e.kind) {
    case Kind::Technique: return MakeTechnique(e.index);
    case Kind::Trap: return MakeTrap(e.index);
    default: return Entry();
    }
}

inline const char* KeyOf(Entry e) {
    e = Clamped(e);
    switch (e.kind) {
    case Kind::Technique: return kTechs[e.index].key;
    case Kind::Trap: return kTraps[e.index].key;
    default: return kEmptyKey;
    }
}

inline const char* NameOf(Entry e) {
    e = Clamped(e);
    switch (e.kind) {
    case Kind::Technique: return kTechs[e.index].name;
    case Kind::Trap: return kTraps[e.index].name;
    default: return kEmptyName;
    }
}

// ---------------------------------------------------------------- text

// Case-insensitive (ASCII) equality.
inline bool SameKey(const std::string& a, const char* b) {
    size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return false;
    return i == a.size() && b[i] == '\0';
}

inline std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace((unsigned char)s[b])) b++;
    while (e > b && isspace((unsigned char)s[e - 1])) e--;
    return s.substr(b, e - b);
}

inline std::string Lower(std::string s) {
    for (char& c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

// A whole-string decimal integer.
inline bool ParseInt(const std::string& s, int& out) {
    const std::string t = Trim(s);
    if (t.empty())
        return false;
    char* end = nullptr;
    errno = 0;
    const long v = strtol(t.c_str(), &end, 10);
    if (errno != 0 || end == nullptr || *end != '\0' || v < -1000000 || v > 1000000)
        return false;
    out = (int)v;
    return true;
}

// The entry an ini key names; false (and empty) for an unknown key.
inline bool FromKey(const std::string& key, Entry& out) {
    out = Entry();
    if (SameKey(key, kEmptyKey))
        return true;
    for (int i = 0; i < kTechCount; i++)
        if (SameKey(key, kTechs[i].key)) {
            out = MakeTechnique(i);
            return true;
        }
    for (int i = 0; i < kTrapCount; i++)
        if (SameKey(key, kTraps[i].key)) {
            out = MakeTrap(i);
            return true;
        }
    return false;
}

// The class byte an ini key names, or -1.
inline int FindClass(const std::string& key) {
    for (int i = 0; i < kClassCount; i++)
        if (SameKey(key, kClasses[i].key))
            return i;
    return -1;
}

struct KeyValue {
    std::string key;
    std::string value;
};

// An ini "key=value" line, both sides trimmed. False for a line without
// '='.
inline bool SplitLine(const std::string& line, KeyValue& out) {
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
        return false;
    out.key = Trim(line.substr(0, eq));
    out.value = Trim(line.substr(eq + 1));
    return true;
}

// Is this [wheel] key a character's line (<prefix>.<field>)? The section's
// other settings have no dot.
inline bool IsLayoutKey(const std::string& key) { return key.find('.') != std::string::npos; }

// ---------------------------------------------------------------- layouts

constexpr int kSegments = 8;
constexpr int kMaxPages = 8;
// Segment names, clockwise from the top; a page's entries are in this
// order.
constexpr const char* kCompass[kSegments] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

struct Page {
    Entry seg[kSegments];
};

struct Layout {
    int pages = 0;
    Page page[kMaxPages];
};

// The file the game writes its characters into for the Options program,
// beside psobbvr.ini: one section per character, named as the [wheel] keys'
// prefix (CharPrefix), holding these keys.
constexpr const char* kRosterFile = "psobbvr-characters.ini";
constexpr const char* kRosterName = "name";    // UTF-8
constexpr const char* kRosterClass = "class";  // a class key
constexpr const char* kRosterLevel = "level";  // as shown in the game

// The highest save slot a key can name (the stock game has four; server
// patches add more).
constexpr int kMaxSlots = 256;

// One character's wheel. slot is the game's own count, from 0.
struct CharLayout {
    unsigned guild_card = 0;
    int slot = 0;
    int cls = -1;      // the class byte the pages were made for; -1 not given
    std::string name;  // UTF-8; not read by the game
    Layout layout;
};

struct Layouts {
    std::vector<CharLayout> chars;
};

inline std::string CharPrefix(unsigned guild_card, int slot) {
    return "gc" + std::to_string(guild_card) + "_slot" + std::to_string(slot + 1);
}

// The character a key's prefix names; false for anything but
// gc<number>_slot<1..kMaxSlots>.
inline bool ParseCharPrefix(const std::string& prefix, unsigned& guild_card, int& slot) {
    const std::string p = Lower(prefix);
    const size_t mid = p.find("_slot");
    if (p.compare(0, 2, "gc") != 0 || mid == std::string::npos || mid <= 2)
        return false;
    const std::string g = p.substr(2, mid - 2), s = p.substr(mid + 5);
    if (g.empty() || g.size() > 10 || s.empty() || s.size() > 4 ||
        g.find_first_not_of("0123456789") != std::string::npos ||
        s.find_first_not_of("0123456789") != std::string::npos)
        return false;
    const unsigned long long gv = strtoull(g.c_str(), nullptr, 10);
    const long sv = strtol(s.c_str(), nullptr, 10);
    if (gv == 0 || gv > 0xFFFFFFFFull || sv < 1 || sv > kMaxSlots)
        return false;
    guild_card = (unsigned)gv;
    slot = (int)sv - 1;
    return true;
}

inline const CharLayout* FindChar(const Layouts& l, unsigned guild_card, int slot) {
    for (const CharLayout& c : l.chars)
        if (c.guild_card == guild_card && c.slot == slot)
            return &c;
    return nullptr;
}
inline CharLayout* FindChar(Layouts& l, unsigned guild_card, int slot) {
    return const_cast<CharLayout*>(FindChar(static_cast<const Layouts&>(l), guild_card, slot));
}

inline std::string PagesKey(const std::string& prefix) { return prefix + ".pages"; }
inline std::string PageKey(const std::string& prefix, int k) {
    return prefix + ".page" + std::to_string(k);
}

// One page line's value into `page`. Warnings name `key`.
inline void ParsePage(const std::string& key, const std::string& value, Page& page,
                      std::vector<std::string>& warnings) {
    page = Page();
    size_t start = 0;
    int seg = 0;
    bool extra = false;
    for (;;) {
        const size_t comma = value.find(',', start);
        const std::string tok =
            Trim(value.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (seg < kSegments) {
            if (!tok.empty() && !FromKey(tok, page.seg[seg]))
                warnings.push_back("'" + key + "': unknown entry '" + tok + "' at " +
                                   kCompass[seg] + " - left empty");
        } else if (!tok.empty()) {
            extra = true;
        }
        seg++;
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    if (extra)
        warnings.push_back("'" + key + "': more than eight entries - the rest ignored");
}

// The section's character lines (IsLayoutKey) into `out`, one CharLayout per
// prefix in the order the prefixes first appear; lines with other keys are
// skipped. Every rejection is appended to `warnings`. A repeated line keeps
// its first value, as the profile functions do.
inline void ParseLayouts(const std::vector<KeyValue>& lines, Layouts& out,
                         std::vector<std::string>& warnings) {
    out = Layouts();
    struct Lines {
        std::string prefix;
        bool has_name = false, has_class = false, has_pages = false;
        int pages = 0;
        bool has_page[kMaxPages] = {};
        std::string page_key[kMaxPages];
        std::string page_value[kMaxPages];
    };
    std::vector<Lines> blocks;  // parallel to out.chars
    int class_lines = 0;
    std::string class_example;
    for (const KeyValue& kv : lines) {
        if (!IsLayoutKey(kv.key))
            continue;
        const size_t dot = kv.key.find('.');
        const std::string prefix = kv.key.substr(0, dot);
        const std::string field = Lower(kv.key.substr(dot + 1));
        unsigned guild_card = 0;
        int slot = 0;
        if (!ParseCharPrefix(prefix, guild_card, slot)) {
            if (FindClass(prefix) >= 0) {
                if (class_lines++ == 0)
                    class_example = kv.key;
            } else {
                warnings.push_back("'" + kv.key + "': '" + prefix +
                                   "' is not a character (gc<guild card>_slot<N>) - line ignored");
            }
            continue;
        }
        size_t b = 0;
        while (b < out.chars.size() &&
               !(out.chars[b].guild_card == guild_card && out.chars[b].slot == slot))
            b++;
        if (b == out.chars.size()) {
            CharLayout c;
            c.guild_card = guild_card;
            c.slot = slot;
            out.chars.push_back(c);
            Lines fresh;
            fresh.prefix = CharPrefix(guild_card, slot);
            blocks.push_back(fresh);
        }
        Lines& L = blocks[b];
        CharLayout& ch = out.chars[b];
        if (field == "name") {
            if (L.has_name)
                warnings.push_back("'" + kv.key + "': repeated - the first line is used");
            else
                ch.name = kv.value;
            L.has_name = true;
            continue;
        }
        if (field == "class") {
            if (L.has_class) {
                warnings.push_back("'" + kv.key + "': repeated - the first line is used");
                continue;
            }
            L.has_class = true;
            ch.cls = FindClass(kv.value);
            if (ch.cls < 0)
                warnings.push_back("'" + kv.key + " = " + kv.value + "': unknown class - ignored");
            continue;
        }
        if (field == "pages") {
            if (L.has_pages) {
                warnings.push_back("'" + kv.key + "': repeated - the first line is used");
                continue;
            }
            int n = 0;
            if (!ParseInt(kv.value, n)) {
                warnings.push_back("'" + kv.key + " = " + kv.value +
                                   "': not a number - line ignored");
                continue;
            }
            if (n < 0 || n > kMaxPages) {
                n = ClampInt(n, 0, kMaxPages);
                warnings.push_back("'" + kv.key + " = " + kv.value + "': out of range (0 to 8), using " +
                                   std::to_string(n));
            }
            L.has_pages = true;
            L.pages = n;
            continue;
        }
        int k = 0;
        const bool page_field = field.size() > 4 && field.compare(0, 4, "page") == 0 &&
                                field.find_first_not_of("0123456789", 4) == std::string::npos &&
                                ParseInt(field.substr(4), k);
        if (!page_field) {
            warnings.push_back("'" + kv.key + "': unknown setting - line ignored");
            continue;
        }
        if (k < 1 || k > kMaxPages) {
            warnings.push_back("'" + kv.key + "': pages are numbered 1 to 8 - line ignored");
            continue;
        }
        if (L.has_page[k - 1]) {
            warnings.push_back("'" + kv.key + "': repeated - the first line is used");
            continue;
        }
        L.has_page[k - 1] = true;
        L.page_key[k - 1] = kv.key;
        L.page_value[k - 1] = kv.value;
    }
    if (class_lines > 0)
        warnings.push_back("'" + class_example + "'" +
                           (class_lines > 1 ? " and " + std::to_string(class_lines - 1) +
                                                  " more line(s) keyed by a class"
                                            : std::string()) +
                           ": wheels are set up per character - ignored");
    for (size_t b = 0; b < blocks.size(); b++) {
        const Lines& L = blocks[b];
        Layout& lay = out.chars[b].layout;
        if (L.has_pages) {
            lay.pages = L.pages;
            std::string missing;
            for (int k = 0; k < kMaxPages; k++) {
                if (k < L.pages && L.has_page[k])
                    ParsePage(L.page_key[k], L.page_value[k], lay.page[k], warnings);
                else if (k < L.pages)
                    missing += (missing.empty() ? "" : ", ") + std::to_string(k + 1);
                else if (L.has_page[k])
                    warnings.push_back("'" + L.page_key[k] + "': past " + PagesKey(L.prefix) + " = " +
                                       std::to_string(L.pages) + " - line ignored");
            }
            if (!missing.empty())
                warnings.push_back(PagesKey(L.prefix) + " = " + std::to_string(L.pages) +
                                   ": no line for page " + missing + " - left empty");
        } else {
            int n = 0;
            bool gap = false;
            for (int k = 0; k < kMaxPages; k++) {
                if (!L.has_page[k]) {
                    gap = true;
                    continue;
                }
                if (gap)
                    warnings.push_back(PageKey(L.prefix, k + 1) +
                                       ": earlier page lines are missing - it becomes page " +
                                       std::to_string(n + 1));
                gap = false;
                ParsePage(L.page_key[k], L.page_value[k], lay.page[n], warnings);
                n++;
            }
            lay.pages = n;
        }
    }
}

// One page as its ini value: its eight keys, comma separated.
inline std::string PageText(const Page& p) {
    std::string s;
    for (int i = 0; i < kSegments; i++) {
        if (i > 0)
            s += ", ";
        s += KeyOf(p.seg[i]);
    }
    return s;
}

// The layouts as [wheel] lines, in the order held: for each character with
// pages, its name and class lines (when known), its pages line, then
// page1..pageN. A character without pages gets no lines. ParseLayouts reads
// the result back unchanged.
inline std::vector<KeyValue> SerializeLayouts(const Layouts& l) {
    std::vector<KeyValue> out;
    for (const CharLayout& c : l.chars) {
        const int n = ClampInt(c.layout.pages, 0, kMaxPages);
        if (n == 0)
            continue;
        const std::string prefix = CharPrefix(c.guild_card, c.slot);
        if (!Trim(c.name).empty())
            out.push_back(KeyValue{prefix + ".name", Trim(c.name)});
        if (c.cls >= 0 && c.cls < kClassCount)
            out.push_back(KeyValue{prefix + ".class", kClasses[c.cls].key});
        out.push_back(KeyValue{PagesKey(prefix), std::to_string(n)});
        for (int k = 0; k < n; k++)
            out.push_back(KeyValue{PageKey(prefix, k + 1), PageText(c.layout.page[k])});
    }
    return out;
}

// ---------------------------------------------------------------- classes

// Maximum-level byte for a technique the class cannot learn.
constexpr unsigned char kCannotLearn = 0xFF;

// The game's maximum-level table as the executable holds it: twelve
// pointers at 0x9CC520 in class-byte order, each to the class's 19-byte
// row (the rows start at 0x9693AC, 0x14 apart) in technique id order. A
// byte is the highest level the class can learn minus one, kCannotLearn
// where it cannot learn the technique at all. Each row below: the nine
// attacks, then grants deband jellen zalure shifta ryuker resta anti
// reverser megid.
constexpr unsigned char kMaxLevels[kClassCount][kTechCount] = {
    {0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E,  // HUmar
     0xFF, 0xFF, 0x0E, 0x0E, 0xFF, 0x0E, 0x0E, 0x04, 0xFF, 0xFF},
    {0x13, 0x13, 0x13, 0x13, 0x13, 0x13, 0x13, 0x13, 0x13,  // HUnewearl
     0xFF, 0x13, 0x13, 0x13, 0x13, 0x13, 0x13, 0x06, 0xFF, 0xFF},
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // HUcast
     0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
    {0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E, 0x0E,  // RAmar
     0xFF, 0x0E, 0xFF, 0xFF, 0x0E, 0x0E, 0x0E, 0x04, 0xFF, 0xFF},
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // RAcast
     0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // RAcaseal
     0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
    {0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D,  // FOmarl
     0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x06, 0x1D, 0x1D},
    {0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D,  // FOnewm
     0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x06, 0x1D, 0x1D},
    {0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D,  // FOnewearl
     0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x06, 0x1D, 0x1D},
    {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,  // HUcaseal
     0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
    {0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D,  // FOmar
     0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x1D, 0x06, 0x1D, 0x1D},
    {0x13, 0x13, 0x13, 0x13, 0x13, 0x13, 0x13, 0x13, 0x13,  // RAmarl
     0xFF, 0x13, 0x13, 0x13, 0x13, 0x13, 0x13, 0x06, 0xFF, 0xFF},
};

// The androids (HUcast, RAcast, RAcaseal, HUcaseal): they learn no
// techniques, and they are the only classes the trap-maximum routine
// (0x698790) gives traps.
constexpr bool IsAndroid(int cls) { return cls == 2 || cls == 4 || cls == 5 || cls == 9; }

// Can the class learn technique `id` (kMaxLevels)?
constexpr bool CanLearn(int cls, int id) {
    return cls >= 0 && cls < kClassCount && id >= 0 && id < kTechCount &&
           kMaxLevels[cls][id] != kCannotLearn;
}

// The trap kind nobody carries: its maximum is 0 for every class outside
// battle rules (0x698790).
constexpr int kSlowTrap = 2;

// Can a segment of the class's wheel hold `e`? `max_levels` is the class's
// row of the maximum-level table (kMaxLevels, or the executable's own).
// Androids carry the Damage, Freeze and Confuse traps and no techniques;
// the other classes the techniques they can learn and no traps.
inline bool CanUse(int cls, Entry e, const unsigned char max_levels[kTechCount]) {
    switch (e.kind) {
    case Kind::None: return true;
    case Kind::Technique:
        return !IsAndroid(cls) && e.index >= 0 && e.index < kTechCount &&
               max_levels[e.index] != kCannotLearn;
    case Kind::Trap: return IsAndroid(cls) && e.index != kSlowTrap;
    }
    return false;
}

// ---------------------------------------------------------------- fill

// Technique ids in the order "fill with what this class can learn" lays
// them out: the attacks first (single target, area, Grants and Megid, then
// the ring tier), then healing, support and the rest.
constexpr int kRoleOrder[kTechCount] = {
    0,  3,  6,  2,  5,  8,  9,  18,  // foie barta zonde rafoie rabarta razonde grants megid
    1,  4,  7,  15, 16, 13, 10, 14,  // gifoie gibarta gizonde resta anti shifta deband ryuker
    11, 12, 17,                      // jellen zalure reverser
};

// What a class can learn as a layout, eight per page in kRoleOrder.
// `max_levels` is the class's row of the game's maximum-level table (19
// bytes in technique id order). A class that learns none (the androids)
// gets one page with the Damage, Freeze and Confuse traps at N, E and W;
// Slow traps are left out, since their maximum is 0 outside battle rules
// (0x698790).
inline Layout FillLearnable(const unsigned char max_levels[kTechCount]) {
    Layout lay;
    int n = 0;
    for (int i = 0; i < kTechCount; i++) {
        const int id = kRoleOrder[i];
        if (max_levels[id] == kCannotLearn)
            continue;
        lay.page[n / kSegments].seg[n % kSegments] = MakeTechnique(id);
        n++;
    }
    if (n > 0) {
        lay.pages = (n + kSegments - 1) / kSegments;
        return lay;
    }
    lay.pages = 1;
    lay.page[0].seg[0] = MakeTrap(0);  // N: Damage
    lay.page[0].seg[2] = MakeTrap(1);  // E: Freeze
    lay.page[0].seg[6] = MakeTrap(3);  // W: Confuse
    return lay;
}

}  // namespace wheelcore
