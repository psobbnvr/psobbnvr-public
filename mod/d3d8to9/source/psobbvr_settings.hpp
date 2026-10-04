#pragma once

// Where the mod's settings come from.
//
// Two files in the game folder:
//   psobbvr.ini           the player's own settings: only the ones they
//                         changed. Releases never ship it, so unzipping an
//                         update cannot overwrite it.
//   psobbvr-defaults.ini  every setting with its default value and what it
//                         does. Each release replaces it.
// A setting comes from psobbvr.ini when the key is there, else from
// psobbvr-defaults.ini, else the reader's built-in default. Readers pass
// the psobbvr.ini path, as they would to GetPrivateProfile*; the defaults
// file is psobbvr-defaults.ini in the same folder.
//
// An install from an older release has a full copy of the shipped file as
// its psobbvr.ini. Tidy() turns it into a player file once: the old file
// is kept as psobbvr.ini.bak, and the new one keeps only the lines that
// differ from the defaults, leaving out old shipped defaults (kOldDefaults)
// and retired settings (kRetired). A file whose first line is
// kUserHeader's is already a player file and is left alone.
//
// Used by both proxy DLLs and the Options program.

#include <windows.h>
#include <share.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace settings {

constexpr const char* kDefaultsName = "psobbvr-defaults.ini";

// Opens every player file; its first line marks the file as one.
constexpr const char* kUserHeader =
    "; Your PSOBB VR mod settings: only the ones you changed.\r\n"
    "; Every setting, what it does and its default value are in\r\n"
    "; psobbvr-defaults.ini. To change one, copy its line from there into the\r\n"
    "; same [section] here. A setting not listed here uses its default.\r\n"
    "; psobbvr_options.exe edits this file for you; updates never replace it.\r\n"
    "\r\n";
constexpr const char* kUserMarker = "; Your PSOBB VR mod settings";

// psobbvr-defaults.ini beside `user_path`.
inline void DefaultsPath(const char* user_path, char* out, size_t cap) {
    snprintf(out, cap, "%s", user_path);
    char* cut = out;
    for (char* p = out; *p != '\0'; p++)
        if (*p == '\\' || *p == '/')
            cut = p + 1;
    *cut = '\0';
    strncat_s(out, cap, kDefaultsName, _TRUNCATE);
}

inline bool FileExists(const char* path) {
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// True when `path` has section/key (an empty value counts). The missing-key
// default is a control character no ini value holds.
inline bool Has(const char* section, const char* key, const char* path) {
    char v[8];
    GetPrivateProfileStringA(section, key, "\x01", v, sizeof(v), path);
    return !(v[0] == '\x01' && v[1] == '\0');
}

// GetPrivateProfileIntA over both files.
inline UINT GetInt(const char* section, const char* key, INT def, const char* user_path) {
    if (Has(section, key, user_path))
        return GetPrivateProfileIntA(section, key, def, user_path);
    char d[MAX_PATH];
    DefaultsPath(user_path, d, sizeof(d));
    return GetPrivateProfileIntA(section, key, def, d);
}

// GetPrivateProfileStringA over both files.
inline DWORD GetString(const char* section, const char* key, const char* def, char* out,
                       DWORD cap, const char* user_path) {
    if (Has(section, key, user_path))
        return GetPrivateProfileStringA(section, key, def, out, cap, user_path);
    char d[MAX_PATH];
    DefaultsPath(user_path, d, sizeof(d));
    return GetPrivateProfileStringA(section, key, def, out, cap, d);
}

// The key of a "key=value" line, trimmed, into `out`.
inline void LineKey(const char* line, char* out, size_t cap) {
    while (*line == ' ' || *line == '\t') line++;
    size_t n = 0;
    while (line[n] != '\0' && line[n] != '=') n++;
    while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) n--;
    if (n >= cap) n = cap - 1;
    memcpy(out, line, n);
    out[n] = '\0';
}

// GetPrivateProfileSectionA over both files: psobbvr.ini's lines, then the
// defaults file's lines for keys psobbvr.ini does not set. Same layout
// ("k=v\0k=v\0\0"); returns the characters written before the final '\0'
// (0 = neither file has a line in the section).
inline DWORD GetSection(const char* section, char* out, DWORD cap, const char* user_path) {
    if (cap < 2)
        return 0;
    out[0] = out[1] = '\0';
    GetPrivateProfileSectionA(section, out, cap, user_path);
    DWORD len = 0;
    while (out[len] != '\0')
        len += (DWORD)strlen(out + len) + 1;
    char d[MAX_PATH];
    DefaultsPath(user_path, d, sizeof(d));
    std::vector<char> dbuf(cap);
    dbuf[0] = dbuf[1] = '\0';
    GetPrivateProfileSectionA(section, dbuf.data(), cap, d);
    for (const char* p = dbuf.data(); *p != '\0'; p += strlen(p) + 1) {
        char key[64];
        LineKey(p, key, sizeof(key));
        bool set = false;
        for (const char* q = out; *q != '\0' && !set; q += strlen(q) + 1) {
            char k2[64];
            LineKey(q, k2, sizeof(k2));
            set = _stricmp(key, k2) == 0;
        }
        const DWORD n = (DWORD)strlen(p) + 1;
        if (set || len + n + 1 > cap)
            continue;
        memcpy(out + len, p, n);
        len += n;
    }
    out[len] = '\0';
    return len;
}

// Make psobbvr.ini a player file before writing to it: a missing file is
// created holding the header. False if it cannot be created.
inline bool EnsureUserFile(const char* user_path) {
    if (FileExists(user_path))
        return true;
    FILE* f = _fsopen(user_path, "wb", _SH_DENYWR);
    if (f == nullptr)
        return false;
    fputs(kUserHeader, f);
    fclose(f);
    return true;
}

// ---------------------------------------------------------------- tidy

// A setting whose shipped default changed. A file without `newer_key` (a
// setting first shipped with the change) predates it, so `value` there is
// the old default, not a choice.
struct OldDefault {
    const char* section;
    const char* key;
    const char* value;
    const char* newer_key;
};
inline constexpr OldDefault kOldDefaults[] = {
    {"vr", "charge_hold_timeout_s", "10", "hud_lock"},
    {"vr", "swing_indicator", "0", "hud_lock"},
    {"vr", "swing_indicator_y", "48", "hud_lock"},
    {"vr", "grip_pitch_deg", "-10", "hud_height_deg"},
};

// Settings earlier releases shipped that the defaults file no longer
// lists, with the values they shipped. The ones the code still reads
// default to these same values.
struct Retired {
    const char* section;
    const char* key;
    const char* value;
};
inline constexpr Retired kRetired[] = {
    {"bindings", "nav_left", "none"},
    {"bindings", "nav_right", "none"},
    {"vr", "combo_cue", "0"},
    {"vr", "combo_cue_amp", "0.6"},
    {"vr", "combo_cue_delay_s", "0.2"},
    {"vr", "combo_cue_gap_s", "0.08"},
    {"vr", "combo_cue_s", "0.03"},
    {"vr", "fist_rest_lock", "1"},
    {"vr", "glow_coverage", "0"},
    {"vr", "gun_aim_origin_hand", "1"},
    {"vr", "gun_fire_pitch_deg", "0"},
    {"vr", "gun_fire_pitch_deg", "15"},
    {"vr", "gun_fire_ray", "1"},
    {"vr", "hand_autocapture", "1"},
    {"vr", "hand_marker", "0"},
    {"vr", "hand_mirror_axis", "0"},
    {"vr", "head_steer_deadzone_deg", "8"},
    {"vr", "head_steer_scale", "0"},
    {"vr", "hide_arms_auto", "1"},
    {"vr", "hide_weapon_trail", "1"},
    {"vr", "hud_drop_deg", "0"},
    {"vr", "hud_mode", "3"},
    {"vr", "mechgun_left_cone_deg", "0"},
    {"vr", "mechgun_left_cone_deg", "15"},
    {"vr", "melee_facing_snap", "1"},
    {"vr", "menu_flat_3d", "1"},
    {"vr", "object_vis_behind", "0"},
    {"vr", "palette_guard", "1"},
    {"vr", "sprite_upright_aspect", "1.5"},
    {"vr", "swing_context", "1"},
    {"vr", "swing_hold", "1"},
    {"vr", "swing_indicator_palette", "1"},
    {"vr", "swing_warp", "1"},
    {"vr", "teleport_hotkeys", "1"},
    {"vr", "teleport_http_port", "12080"},
    {"vr", "trail_cross_mode", "1"},
    {"vr", "tune_hotkeys", "0"},
    {"vr", "twin_split_kinds", "1"},
    {"vr", "weapon_seat_bone", "1"},
    {"vr", "yaw_follow", "1"},
};

// Equal as numbers when both are numbers, else as text (any case).
inline bool SameValue(const char* a, const char* b) {
    char* ea = nullptr;
    char* eb = nullptr;
    const double x = strtod(a, &ea);
    const double y = strtod(b, &eb);
    if (ea != a && *ea == '\0' && eb != b && *eb == '\0')
        return fabs(x - y) < 1e-6;
    return _stricmp(a, b) == 0;
}

struct Entry {
    char section[64];
    char key[64];
    char value[512];
};

inline void TrimInPlace(char*& s) {
    while (*s == ' ' || *s == '\t') s++;
    char* e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
}

// Every "key=value" line of an ini, in order, as the profile functions
// read it (comments and lines outside a section skipped, matching quotes
// stripped). False if the file cannot be opened.
inline bool ReadEntries(const char* path, std::vector<Entry>& out) {
    FILE* f = _fsopen(path, "rb", _SH_DENYNO);
    if (f == nullptr)
        return false;
    char line[1024];
    char section[64] = "";
    bool first = true;
    while (fgets(line, sizeof(line), f) != nullptr) {
        char* s = line;
        if (first && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
            (unsigned char)s[2] == 0xBF)
            s += 3;
        first = false;
        TrimInPlace(s);
        if (*s == '\0' || *s == ';' || *s == '#')
            continue;
        if (*s == '[') {
            char* close = strchr(s, ']');
            if (close != nullptr) {
                *close = '\0';
                char* name = s + 1;
                TrimInPlace(name);
                snprintf(section, sizeof(section), "%s", name);
            }
            continue;
        }
        char* eq = strchr(s, '=');
        if (eq == nullptr || section[0] == '\0')
            continue;
        *eq = '\0';
        char* key = s;
        char* value = eq + 1;
        TrimInPlace(key);
        TrimInPlace(value);
        const size_t vn = strlen(value);
        if (vn >= 2 && value[0] == '"' && value[vn - 1] == '"') {
            value[vn - 1] = '\0';
            value++;
        }
        Entry e;
        snprintf(e.section, sizeof(e.section), "%s", section);
        snprintf(e.key, sizeof(e.key), "%s", key);
        snprintf(e.value, sizeof(e.value), "%s", value);
        out.push_back(e);
    }
    fclose(f);
    return true;
}

inline const Entry* FindEntry(const std::vector<Entry>& v, const char* section,
                              const char* key) {
    for (const Entry& e : v)
        if (_stricmp(e.section, section) == 0 && _stricmp(e.key, key) == 0)
            return &e;
    return nullptr;
}

// Does the line still say something the defaults do not?
inline bool KeepEntry(const Entry& e, const std::vector<Entry>& file,
                      const std::vector<Entry>& defaults) {
    // A repeated key: the profile functions only ever read the first.
    if (FindEntry(file, e.section, e.key) != &e)
        return false;
    if (const Entry* d = FindEntry(defaults, e.section, e.key))
        if (SameValue(e.value, d->value))
            return false;
    for (const OldDefault& o : kOldDefaults)
        if (_stricmp(e.section, o.section) == 0 && _stricmp(e.key, o.key) == 0 &&
            SameValue(e.value, o.value) && FindEntry(file, o.section, o.newer_key) == nullptr)
            return false;
    if (FindEntry(defaults, e.section, e.key) == nullptr)
        for (const Retired& r : kRetired)
            if (_stricmp(e.section, r.section) == 0 && _stricmp(e.key, r.key) == 0 &&
                SameValue(e.value, r.value))
                return false;
    return true;
}

struct TidyResult {
    bool tidied = false;
    int kept = 0;
    int dropped = 0;
    char note[256] = "";   // for the log; empty when there was nothing to do
};

inline bool IsUserFile(const char* user_path) {
    FILE* f = _fsopen(user_path, "rb", _SH_DENYNO);
    if (f == nullptr)
        return false;
    char line[128] = "";
    if (fgets(line, sizeof(line), f) == nullptr)
        line[0] = '\0';
    fclose(f);
    const char* s = line;
    if ((unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        s += 3;
    return strncmp(s, kUserMarker, strlen(kUserMarker)) == 0;
}

// Turn an older full psobbvr.ini into a player file (see the top of this
// file). Does nothing without psobbvr.ini, without the defaults file to
// compare against, or when psobbvr.ini is already a player file.
inline TidyResult Tidy(const char* user_path) {
    TidyResult r;
    if (!FileExists(user_path) || IsUserFile(user_path))
        return r;
    char defaults_path[MAX_PATH];
    DefaultsPath(user_path, defaults_path, sizeof(defaults_path));
    std::vector<Entry> file, defaults;
    if (!ReadEntries(defaults_path, defaults)) {
        snprintf(r.note, sizeof(r.note),
                 "%s not found - psobbvr.ini left as it is", kDefaultsName);
        return r;
    }
    if (!ReadEntries(user_path, file)) {
        snprintf(r.note, sizeof(r.note), "psobbvr.ini unreadable - left as it is");
        return r;
    }
    // Kept lines, grouped by section in the order the sections first appear.
    std::vector<const Entry*> kept;
    for (const Entry& e : file) {
        if (KeepEntry(e, file, defaults))
            kept.push_back(&e);
        else
            r.dropped++;
    }
    r.kept = (int)kept.size();
    char new_path[MAX_PATH + 8], bak_path[MAX_PATH + 8];
    snprintf(new_path, sizeof(new_path), "%s.new", user_path);
    snprintf(bak_path, sizeof(bak_path), "%s.bak", user_path);
    FILE* f = _fsopen(new_path, "wb", _SH_DENYWR);
    if (f == nullptr) {
        snprintf(r.note, sizeof(r.note), "could not write %s (error %lu) - psobbvr.ini left as it is",
                 new_path, GetLastError());
        return r;
    }
    fputs(kUserHeader, f);
    std::vector<const char*> sections;
    for (const Entry* e : kept) {
        bool seen = false;
        for (const char* s : sections)
            seen = seen || _stricmp(s, e->section) == 0;
        if (!seen)
            sections.push_back(e->section);
    }
    for (const char* s : sections) {
        fprintf(f, "%s[%s]\r\n", s == sections.front() ? "" : "\r\n", s);
        for (const Entry* e : kept)
            if (_stricmp(e->section, s) == 0)
                fprintf(f, "%s=%s\r\n", e->key, e->value);
    }
    const bool wrote = fflush(f) == 0;
    fclose(f);
    if (!wrote || !CopyFileA(user_path, bak_path, FALSE) ||
        !MoveFileExA(new_path, user_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        snprintf(r.note, sizeof(r.note), "could not replace psobbvr.ini (error %lu) - left as it is",
                 GetLastError());
        DeleteFileA(new_path);
        return r;
    }
    // Drop any copy of the old file the profile functions still hold.
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, user_path);
    r.tidied = true;
    snprintf(r.note, sizeof(r.note),
             "psobbvr.ini now holds only your changes: kept %d setting(s), left %d at "
             "their defaults; the old file is psobbvr.ini.bak",
             r.kept, r.dropped);
    return r;
}

}  // namespace settings
