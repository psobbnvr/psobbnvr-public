// The settings-key redirect. See registry_redirect.h.
//
// How the game reaches its key: one routine (0x82D444 read / 0x82D5A4
// write) opens three nested keys with RegOpenKeyExA - HKCU + "Software"
// (literal 0x98A3C4), the company, the product - and keeps the innermost
// handle at 0xACBF00. The company and product names are copied at startup
// (0x82D650, called from 0x7A60A1) from "SONICTEAM" (0x97D0D8) and "PSOBB"
// (0x97D0E4) into buffers at 0xA216C0 / 0xA21700. A second cluster near
// the crypto routines (0x8C10xx-0x8C76xx, RegCreateKeyA/ExA under HKCU) is
// the account storage. The game imports only the ANSI registry API.
//
// The rule: in any open/create under HKCU or under a handle (how the
// nested opener passes the company segment), a path segment equal to
// "SonicTeam" (any case) becomes "psobbvr". It is idempotent, so a call
// that reaches two hooks (RegOpenKeyA into RegOpenKeyExA) is harmless.
// HKLM/HKCR are never touched.
#include <windows.h>
#include <cstdint>
#include <cstring>

#include "MinHook.h"
#include "common/log.h"
#include "common/registry_seed.h"
#include "dinput8/registry_redirect.h"

namespace regredirect {
namespace {

constexpr const char* kOldCompany = "SonicTeam";
constexpr const char* kNewCompany = "psobbvr";
constexpr const char* kOldSettingsKey = "Software\\SonicTeam\\PSOBB";
constexpr const char* kNewSettingsKey = "Software\\psobbvr\\PSOBB";
constexpr size_t kPathCap = MAX_PATH * 2;

bool g_active = false;

// Each distinct (api, root, access, path) is logged once and counted after
// that (the game re-opens its key for nearly every value). Summary at
// process detach.
struct Seen {
    char api[20];
    char root[10];
    DWORD sam;          // access mask asked for (0 for the old RegOpenKeyA/RegCreateKeyA)
    bool redirected;    // false = an HKCU open we observed and passed through
    char from[kPathCap];
    char to[kPathCap];
    LONG count;
};
constexpr int kSeenCap = 24;
Seen g_seen[kSeenCap];
int g_seen_count = 0;
volatile LONG g_seen_overflow = 0;
CRITICAL_SECTION g_seen_lock;
bool g_seen_lock_ready = false;

using RegOpenKeyExAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
using RegOpenKeyAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, PHKEY);
using RegCreateKeyExAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, DWORD, LPSTR, DWORD, REGSAM,
                                            const SECURITY_ATTRIBUTES*, PHKEY, LPDWORD);
using RegCreateKeyAFn = LSTATUS(WINAPI*)(HKEY, LPCSTR, PHKEY);

RegOpenKeyExAFn orig_open_ex = nullptr;
RegOpenKeyAFn orig_open = nullptr;
RegCreateKeyExAFn orig_create_ex = nullptr;
RegCreateKeyAFn orig_create = nullptr;

bool IsPredefined(HKEY k) {
    return (reinterpret_cast<uintptr_t>(k) & 0x80000000u) != 0;
}

// HKCU itself, or a real handle (the nested opener's "Software" key).
bool RootEligible(HKEY k) {
    return k == HKEY_CURRENT_USER || !IsPredefined(k);
}

const char* RootName(HKEY k) {
    if (k == HKEY_CURRENT_USER) return "HKCU";
    if (k == HKEY_LOCAL_MACHINE) return "HKLM";
    if (k == HKEY_CLASSES_ROOT) return "HKCR";
    return IsPredefined(k) ? "HK??" : "<handle>";
}

// Copy `sub` into `out`, replacing every backslash-separated segment that
// equals the old company name (case-insensitive). True when something
// changed; false (out untouched) otherwise or when it would not fit.
bool Rewrite(const char* sub, char* out, size_t cap) {
    if (sub == nullptr) return false;
    const size_t n = strlen(sub);
    if (n == 0 || n + 16 >= cap) return false;
    const size_t old_len = strlen(kOldCompany);
    const size_t new_len = strlen(kNewCompany);
    bool changed = false;
    size_t o = 0;
    size_t i = 0;
    while (i <= n) {
        size_t j = i;
        while (j < n && sub[j] != '\\') j++;
        const size_t len = j - i;
        if (len == old_len && _strnicmp(sub + i, kOldCompany, len) == 0) {
            if (o + new_len + 1 >= cap) return false;
            memcpy(out + o, kNewCompany, new_len);
            o += new_len;
            changed = true;
        } else {
            if (o + len + 1 >= cap) return false;
            memcpy(out + o, sub + i, len);
            o += len;
        }
        if (j < n) out[o++] = '\\';
        i = j + 1;
    }
    out[o] = '\0';
    return changed;
}

const char* SamName(DWORD sam) {
    if (sam == 0) return "n/a";
    if (sam == KEY_READ) return "read";
    if (sam == KEY_WRITE) return "write";
    if (sam == (KEY_READ | KEY_WRITE)) return "read+write";
    if (sam == KEY_ALL_ACCESS) return "all";
    return "other";
}

// Record one open under HKCU (or a handle): redirected ones carry the new
// path in `to`, observed pass-throughs have to == nullptr. Distinct on
// (api, root, access, path).
void Note(const char* api, HKEY root, DWORD sam, const char* from, const char* to) {
    if (from == nullptr) from = "";
    const char* root_name = RootName(root);
    if (!g_seen_lock_ready) return;
    EnterCriticalSection(&g_seen_lock);
    bool found = false;
    for (int i = 0; i < g_seen_count; i++) {
        Seen& s = g_seen[i];
        if (strcmp(s.api, api) == 0 && strcmp(s.root, root_name) == 0 && s.sam == sam &&
            _stricmp(s.from, from) == 0) {
            s.count++;
            found = true;
            break;
        }
    }
    if (!found) {
        if (g_seen_count < kSeenCap && strlen(from) < kPathCap) {
            Seen& s = g_seen[g_seen_count++];
            strcpy_s(s.api, api);
            strcpy_s(s.root, root_name);
            s.sam = sam;
            s.redirected = (to != nullptr);
            strcpy_s(s.from, from);
            strcpy_s(s.to, to != nullptr ? to : "");
            s.count = 1;
            if (to != nullptr)
                logging::Line("registry: redirect %s %s %s\\%s -> %s\\%s (first sighting)", api,
                              SamName(sam), root_name, from, root_name, to);
            else
                logging::Line("registry: observed %s %s %s\\%s (passed through, first sighting)",
                              api, SamName(sam), root_name, from);
        } else {
            InterlockedIncrement(&g_seen_overflow);
        }
    }
    LeaveCriticalSection(&g_seen_lock);
}

LSTATUS WINAPI HookOpenEx(HKEY root, LPCSTR sub, DWORD options, REGSAM sam, PHKEY out) {
    char buf[kPathCap];
    if (RootEligible(root)) {
        if (Rewrite(sub, buf, sizeof(buf))) {
            Note("RegOpenKeyExA", root, sam, sub, buf);
            return orig_open_ex(root, buf, options, sam, out);
        }
        Note("RegOpenKeyExA", root, sam, sub, nullptr);
    }
    return orig_open_ex(root, sub, options, sam, out);
}

LSTATUS WINAPI HookOpen(HKEY root, LPCSTR sub, PHKEY out) {
    char buf[kPathCap];
    if (RootEligible(root)) {
        if (Rewrite(sub, buf, sizeof(buf))) {
            Note("RegOpenKeyA", root, 0, sub, buf);
            return orig_open(root, buf, out);
        }
        Note("RegOpenKeyA", root, 0, sub, nullptr);
    }
    return orig_open(root, sub, out);
}

LSTATUS WINAPI HookCreateEx(HKEY root, LPCSTR sub, DWORD reserved, LPSTR cls, DWORD options,
                            REGSAM sam, const SECURITY_ATTRIBUTES* sa, PHKEY out,
                            LPDWORD disposition) {
    char buf[kPathCap];
    if (RootEligible(root)) {
        if (Rewrite(sub, buf, sizeof(buf))) {
            Note("RegCreateKeyExA", root, sam, sub, buf);
            return orig_create_ex(root, buf, reserved, cls, options, sam, sa, out, disposition);
        }
        Note("RegCreateKeyExA", root, sam, sub, nullptr);
    }
    return orig_create_ex(root, sub, reserved, cls, options, sam, sa, out, disposition);
}

LSTATUS WINAPI HookCreate(HKEY root, LPCSTR sub, PHKEY out) {
    char buf[kPathCap];
    if (RootEligible(root)) {
        if (Rewrite(sub, buf, sizeof(buf))) {
            Note("RegCreateKeyA", root, 0, sub, buf);
            return orig_create(root, buf, out);
        }
        Note("RegCreateKeyA", root, 0, sub, nullptr);
    }
    return orig_create(root, sub, out);
}

// ---- seeding helpers ----------------------------------------------------

// The fresh-key values are in common/registry_seed.h (shared with the
// options program). GRAPHICCTRL's clip distance is 2, the stock maximum:
// the game's loader (0x482B38) clamps every field, clip to 0..2, and
// draw_distance_scale was tuned on 2. CTRLBUF all zero = the 640x480
// request the resolution override replaces. Only missing values are
// written, every launch: the options program can create the key first,
// and the game will not start without CTRLBUF.

// Last-write time of a key (0 on failure). The game never writes its
// preferences; option.exe does, into the shared key (it loads none of our
// DLLs). So when the shared key is newer than ours, its benign
// preferences are copied over.
ULONGLONG LastWrite(HKEY key) {
    FILETIME ft = {};
    if (RegQueryInfoKeyA(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         nullptr, nullptr, nullptr, &ft) != ERROR_SUCCESS)
        return 0;
    return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// Copy one value when it has the expected type (and size, when given - a
// foreign blob must match the stock layout). Returns 1 when copied.
int CopyValue(HKEY from, HKEY to, const char* name, DWORD want_type, DWORD want_size) {
    BYTE data[512];
    DWORD size = sizeof(data);
    DWORD type = 0;
    if (RegQueryValueExA(from, name, nullptr, &type, data, &size) != ERROR_SUCCESS)
        return 0;
    if (type != want_type || (want_size != 0 && size != want_size)) {
        logging::Line("registry: seed skipped %s from the shared key (type %lu size %lu, wanted %lu/%lu)",
                      name, (unsigned long)type, (unsigned long)size,
                      (unsigned long)want_type, (unsigned long)want_size);
        return 0;
    }
    // Other launchers' clip distances 3..6 need no fixing: the game's
    // loader clamps them to 0..2.
    return RegSetValueExA(to, name, 0, type, data, size) == ERROR_SUCCESS ? 1 : 0;
}

} // namespace

void Install(bool enabled) {
    if (!enabled) {
        logging::Line("registry: redirect OFF ([registry] redirect=0) - the game uses the shared key HKCU\\%s",
                      kOldSettingsKey);
        return;
    }
    HMODULE adv = GetModuleHandleA("advapi32.dll");
    if (adv == nullptr) adv = LoadLibraryA("advapi32.dll");
    if (adv == nullptr) {
        logging::Line("registry: redirect FAILED - advapi32.dll not found; using the shared key");
        return;
    }
    struct Target {
        const char* name;
        void* hook;
        void** orig;
    };
    // GetProcAddress resolves forwarders, so this is the address the game's
    // import table holds.
    const Target targets[] = {
        {"RegOpenKeyExA", reinterpret_cast<void*>(&HookOpenEx), reinterpret_cast<void**>(&orig_open_ex)},
        {"RegOpenKeyA", reinterpret_cast<void*>(&HookOpen), reinterpret_cast<void**>(&orig_open)},
        {"RegCreateKeyExA", reinterpret_cast<void*>(&HookCreateEx), reinterpret_cast<void**>(&orig_create_ex)},
        {"RegCreateKeyA", reinterpret_cast<void*>(&HookCreate), reinterpret_cast<void**>(&orig_create)},
    };
    void* enabled_targets[4] = {};
    int enabled_count = 0;
    for (const Target& t : targets) {
        void* target = reinterpret_cast<void*>(GetProcAddress(adv, t.name));
        MH_STATUS st = target ? MH_CreateHook(target, t.hook, t.orig) : MH_ERROR_FUNCTION_NOT_FOUND;
        if (st == MH_OK) st = MH_EnableHook(target);
        if (st != MH_OK) {
            logging::Line("registry: redirect FAILED hooking %s (%s); using the shared key",
                          t.name, MH_StatusToString(st));
            for (int i = 0; i < enabled_count; i++) MH_DisableHook(enabled_targets[i]);
            return;
        }
        enabled_targets[enabled_count++] = target;
    }
    InitializeCriticalSection(&g_seen_lock);
    g_seen_lock_ready = true;
    g_active = true;
    logging::Line("registry: redirect ON - HKCU\\Software\\%s\\* opens go to HKCU\\Software\\%s\\* (4 hooks)",
                  kOldCompany, kNewCompany);
}

bool Active() { return g_active; }

void LogSummary() {
    if (!g_active || !g_seen_lock_ready) return;
    EnterCriticalSection(&g_seen_lock);
    logging::Line("registry: summary - %d distinct HKCU/handle opens this session%s", g_seen_count,
                  g_seen_overflow ? " (table full, some distinct paths uncounted)" : "");
    for (int i = 0; i < g_seen_count; i++) {
        const Seen& s = g_seen[i];
        if (s.redirected)
            logging::Line("registry:   %6ld x %s %s %s\\%s -> %s\\%s (redirected)", (long)s.count,
                          s.api, SamName(s.sam), s.root, s.from, s.root, s.to);
        else
            logging::Line("registry:   %6ld x %s %s %s\\%s (passed through)", (long)s.count,
                          s.api, SamName(s.sam), s.root, s.from);
    }
    LeaveCriticalSection(&g_seen_lock);
}

const char* SettingsKeyPath() { return g_active ? kNewSettingsKey : kOldSettingsKey; }

void SeedOrSync() {
    if (!g_active) return;
    HKEY key = nullptr;
    DWORD disposition = 0;
    // The originals: the new path needs no rewrite and the old key must be
    // reached as itself.
    LSTATUS st = orig_create_ex(HKEY_CURRENT_USER, kNewSettingsKey, 0, nullptr, 0,
                                KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, &disposition);
    if (st != ERROR_SUCCESS) {
        logging::Line("registry: cannot create HKCU\\%s (error %ld)", kNewSettingsKey, (long)st);
        return;
    }
    const bool fresh = (disposition == REG_CREATED_NEW_KEY);
    char seeded_names[512];
    const int seeded = regseed::SeedMissingStockValues(key, seeded_names, sizeof(seeded_names));
    if (!fresh && seeded > 0)
        logging::Line("registry: HKCU\\%s existed without %d stock value(s) - seeded: %s "
                      "(the game will not start without CTRLBUF)",
                      kNewSettingsKey, seeded, seeded_names);

    HKEY old = nullptr;
    if (orig_open_ex(HKEY_CURRENT_USER, kOldSettingsKey, 0, KEY_QUERY_VALUE, &old) != ERROR_SUCCESS) {
        logging::Line(fresh ? "registry: seeded HKCU\\%s from the stock defaults (no shared key on this machine)"
                            : "registry: settings key HKCU\\%s (redirect active; no shared key to sync from)",
                      kNewSettingsKey);
        RegCloseKey(key);
        return;
    }
    const ULONGLONG old_time = LastWrite(old);
    const ULONGLONG our_time = LastWrite(key);
    // Fresh key: always copy. Existing key: only when the shared key was
    // written after ours; copying bumps our write time, so once per change.
    // Never copied: WINDOW_MODE and CTRLBUF (they break VR), the account
    // fields and site URLs (another server's).
    if (fresh || old_time > our_time) {
        int copied = 0;
        copied += CopyValue(old, key, "GRAPHICCTRL", REG_BINARY, 36);
        copied += CopyValue(old, key, "SOUNDCTRL", REG_BINARY, 12);
        copied += CopyValue(old, key, "FONT_JPN", REG_SZ, 0);
        copied += CopyValue(old, key, "FOCUS_SOUND", REG_DWORD, 4);
        copied += CopyValue(old, key, "WORD_WRAP", REG_DWORD, 4);
        copied += CopyValue(old, key, "ACCOUNT_CHECK", REG_DWORD, 4);
        if (fresh)
            logging::Line("registry: seeded HKCU\\%s from the stock defaults + %d preferences copied from the shared key",
                          kNewSettingsKey, copied);
        else
            logging::Line("registry: shared key written %.1f min after ours (option.exe or another install) - %d preferences copied over (graphics, sound, font, focus sound, word wrap, save-account tick)",
                          (old_time - our_time) / 6e8, copied);
    } else {
        logging::Line("registry: settings key HKCU\\%s (redirect active; shared key older, nothing to sync)",
                      kNewSettingsKey);
    }
    RegCloseKey(old);
    RegCloseKey(key);
}

} // namespace regredirect
