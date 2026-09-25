// Stock values of the game's settings key, written only where a value is
// missing. Shared by the dinput8 launch seed and the options program: the
// options program can create the key before the game's first launch, and
// the game refuses to start when CTRLBUF is absent (its D3D init fails,
// one silent retry, exit code 1).
#pragma once

#include <windows.h>
#include <cstring>
#include <cstdio>

namespace regseed {

// Writes every stock value that is not present in `key`. Returns the count
// written and lists their names in `names` (comma-separated) when given.
// GRAPHICCTRL is the VR-tuned blob (custom tier, advanced effects off,
// shadow Low, enemy High, map Medium, clip Far, vertex fog, low-res off,
// frame skip 0); everything else is the stock install seed.
inline int SeedMissingStockValues(HKEY key, char* names = nullptr, size_t names_len = 0) {
    static const BYTE ctrlbuf[12] = {};
    static const DWORD graphics[9] = {3, 0, 0, 1, 1, 2, 0, 0, 0};
    static const DWORD sound[3] = {1, 1, 1};
    static const BYTE account_ctrl[8] = {0x3d, 0xaa, 0xd0, 0x6e, 0xae, 0x64, 0xcd, 0x48};
    static const DWORD zero = 0, one = 1, two = 2, client_code = 0xe;
    struct Entry {
        const char* name;
        DWORD type;
        const void* data;
        DWORD len;  // 0 for REG_SZ: strlen + 1
    };
    static const Entry entries[] = {
        {"CTRLBUF", REG_BINARY, ctrlbuf, sizeof(ctrlbuf)},
        {"GRAPHICCTRL", REG_BINARY, graphics, sizeof(graphics)},
        {"SOUNDCTRL", REG_BINARY, sound, sizeof(sound)},
        {"FONT_JPN", REG_SZ, "Dotum", 0},
        {"ACCOUNT_CHECK", REG_DWORD, &zero, sizeof(DWORD)},
        {"WINDOW_MODE", REG_DWORD, &one, sizeof(DWORD)},
        {"FOCUS_SOUND", REG_DWORD, &one, sizeof(DWORD)},
        {"WORD_WRAP", REG_DWORD, &one, sizeof(DWORD)},
        {"ACCOUNT", REG_SZ, "", 0},
        {"PASSWORD", REG_SZ, "", 0},
        {"INSTALL", REG_DWORD, &zero, sizeof(DWORD)},
        {"CLIENT_CODE", REG_DWORD, &client_code, sizeof(DWORD)},
        {"BILLING_SITE", REG_SZ, "http://www.pioneer2.net/forum/", 0},
        {"OldCheck", REG_DWORD, &zero, sizeof(DWORD)},
        {"EXT0", REG_DWORD, &two, sizeof(DWORD)},
        {"OFFICIAL_SITE", REG_SZ, "http://www.pioneer2.net/forum/", 0},
        {"ACCOUNT_CTRL", REG_BINARY, account_ctrl, sizeof(account_ctrl)},
    };
    if (names && names_len) names[0] = '\0';
    int written = 0;
    for (const Entry& e : entries) {
        if (RegQueryValueExA(key, e.name, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS)
            continue;
        const DWORD len = e.type == REG_SZ
                              ? static_cast<DWORD>(strlen(static_cast<const char*>(e.data)) + 1)
                              : e.len;
        if (RegSetValueExA(key, e.name, 0, e.type, static_cast<const BYTE*>(e.data), len) !=
            ERROR_SUCCESS)
            continue;
        if (names && names_len) {
            const size_t used = strlen(names);
            snprintf(names + used, names_len - used, "%s%s", written ? ", " : "", e.name);
        }
        written++;
    }
    return written;
}

} // namespace regseed
