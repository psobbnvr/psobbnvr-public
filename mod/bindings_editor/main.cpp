// PSOBB VR Options: replaces the stock option.exe, which cannot save on
// modern systems (its resolution list is empty).
//
//   1. Controller bindings - psobbvr.ini [bindings], checked with the
//      mod's own parser (psobbvr_bindings_core.hpp). A running game
//      reloads them within a second of Save.
//   2. Game options - the GRAPHICCTRL / SOUNDCTRL blobs and save-account
//      flag, written to HKCU\Software\psobbvr\PSOBB (the SonicTeam key
//      with [registry] redirect=0). Read by the game at launch.
//   3. VR settings - selected psobbvr.ini keys, read by the mod at
//      launch.
//
// Reads psobbvr.ini next to its exe unless a path is given on the
// command line. Plain Win32, static CRT.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <commdlg.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "psobbvr_bindings_core.hpp"
#include "common/registry_seed.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace bindcore;

namespace {

// ---------------------------------------------------------------- common

char g_ini_path[MAX_PATH] = "";
HWND g_main = nullptr;
HWND g_tab = nullptr;
HWND g_log = nullptr;
HWND g_status = nullptr;
HFONT g_font = nullptr;
HFONT g_font_bold = nullptr;
HFONT g_font_mono = nullptr;

enum Page { PAGE_BINDINGS = 0, PAGE_GAME = 1, PAGE_VR = 2, PAGE_COUNT = 3 };
HWND g_page[PAGE_COUNT] = {};
int g_page_index = PAGE_BINDINGS;
// Page scrolling, in screen pixels: each page's content height, the
// visible height all pages share, and each page's scroll position.
int g_page_content[PAGE_COUNT] = {};
int g_page_view = 0;
int g_page_scroll[PAGE_COUNT] = {};

// Layout constants and coordinates are in 96-DPI units; the Make*
// helpers and BuildUi scale them to the screen's DPI.
int g_dpi = 96;
int S(int v) { return MulDiv(v, g_dpi, 96); }

// Control ids. Bindings combos 1000.., common buttons 2000.., game page
// 3000.., VR page 4000.. (one id per knob; the resolution pair shares one).
enum {
    ID_FIRST_COMBO = 1000,
    ID_CHECK = 2001, ID_SAVE = 2002, ID_DEFAULTS = 2003, ID_RELOAD = 2004, ID_OPENINI = 2005,
    ID_EXPORT = 2007, ID_IMPORT = 2008,
    ID_G_PRESET = 3000, ID_G_SHADOW, ID_G_ENEMY, ID_G_MAP, ID_G_CLIP, ID_G_FOG, ID_G_ADVANCED,
    ID_G_LOWRES, ID_G_FRAMESKIP, ID_G_AUTOFS, ID_G_BGM, ID_G_SE, ID_G_SAVEACCOUNT,
    ID_VR_FIRST = 4000
};

constexpr int MARGIN = 12;
constexpr int PAGE_W = 860;      // page client width
constexpr int LOG_H = 110;
constexpr int TAB_H = 26;

void AppendLog(const char* text) {
    const int len = GetWindowTextLengthA(g_log);
    SendMessageA(g_log, EM_SETSEL, len, len);
    SendMessageA(g_log, EM_REPLACESEL, FALSE, (LPARAM)text);
    SendMessageA(g_log, EM_REPLACESEL, FALSE, (LPARAM)"\r\n");
}
void ClearLog() { SetWindowTextA(g_log, ""); }
void LogCb(void*, const char* line) { AppendLog(line); }
void SetStatus(const char* s) { SetWindowTextA(g_status, s); }

HWND MakeLabel(HWND parent, const char* text, int x, int y, int w, int h, HFONT font) {
    HWND h_ = CreateWindowExA(0, "STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, S(x), S(y), S(w),
                              S(h), parent, nullptr, nullptr, nullptr);
    SendMessageA(h_, WM_SETFONT, (WPARAM)font, TRUE);
    return h_;
}

HWND MakeButton(HWND parent, const char* text, int id, int x, int y, int w, int h) {
    HWND b = CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                             S(x), S(y), S(w), S(h), parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageA(b, WM_SETFONT, (WPARAM)g_font, TRUE);
    return b;
}

HWND MakeCheck(HWND parent, const char* text, int id, int x, int y, int w) {
    HWND b = CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                             S(x), S(y), S(w), S(20), parent, (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageA(b, WM_SETFONT, (WPARAM)g_font, TRUE);
    return b;
}

HWND MakeDropList(HWND parent, int id, int x, int y, int w, const char* const* items, int n) {
    HWND c = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                             CBS_DROPDOWNLIST, S(x), S(y), S(w), S(300), parent,
                             (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageA(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    for (int i = 0; i < n; i++) SendMessageA(c, CB_ADDSTRING, 0, (LPARAM)items[i]);
    return c;
}

HWND MakeEdit(HWND parent, int id, int x, int y, int w) {
    HWND e = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                             ES_AUTOHSCROLL, S(x), S(y), S(w), S(22), parent, (HMENU)(INT_PTR)id,
                             nullptr, nullptr);
    SendMessageA(e, WM_SETFONT, (WPARAM)g_font, TRUE);
    return e;
}

HWND MakeGroup(HWND parent, const char* text, int x, int y, int w, int h) {
    HWND g = CreateWindowExA(0, "BUTTON", text, WS_CHILD | WS_VISIBLE | BS_GROUPBOX, S(x), S(y),
                             S(w), S(h), parent, nullptr, nullptr, nullptr);
    SendMessageA(g, WM_SETFONT, (WPARAM)g_font_bold, TRUE);
    return g;
}

bool Checked(HWND h) { return SendMessageA(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }
void SetChecked(HWND h, bool v) { SendMessageA(h, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0); }
int Sel(HWND h) { return (int)SendMessageA(h, CB_GETCURSEL, 0, 0); }
void SetSel(HWND h, int i) { SendMessageA(h, CB_SETCURSEL, i, 0); }

// ------------------------------------------------------- page 1: bindings

HWND g_combo[kActionCount] = {};
// Notes above certain combos - actions that are deliberately unbound by
// default and would only duplicate something if set. A tracking tooltip
// per noted combo: shown while that combo has the focus (a click on its
// text box or its arrow), hidden when the focus leaves.
struct ComboNote { const char* action; const char* text; };
const char* const kNavNote =
    "Not bound by default on purpose: in menus the sticks already move the cursor, "
    "so a button here would only duplicate them and take that button away from "
    "everything else. Set it only if you want a dedicated button for this arrow.";
const ComboNote kComboNotes[] = {
    {"quick_menu",
     "Setting this will cause there to be two Quick Menu bindings: the one you set here, "
     "and bank2 + quick_chat (right grip + A by default), which the game itself turns "
     "into the Quick Menu. Leave it at none unless you want both."},
    {"nav_left", kNavNote},
    {"nav_right", kNavNote},
    {"nav_up", kNavNote},
    {"nav_down", kNavNote},
};
constexpr int kComboNoteCount = (int)(sizeof(kComboNotes) / sizeof(kComboNotes[0]));
struct NoteState { HWND combo = nullptr; HWND tip = nullptr; const char* text = nullptr; };
NoteState g_notes[kComboNoteCount];
int g_note_up = -1;  // index of the note currently shown, -1 = none

int NoteIndexOf(HWND combo) {
    for (int i = 0; i < kComboNoteCount; i++)
        if (g_notes[i].combo && g_notes[i].combo == combo)
            return i;
    return -1;
}

TOOLINFOA NoteTool(int i, HWND page) {
    TOOLINFOA ti = {};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_TRACK | TTF_ABSOLUTE;
    ti.hwnd = page;
    ti.uId = (UINT_PTR)g_notes[i].combo;
    ti.lpszText = (LPSTR)g_notes[i].text;
    return ti;
}

void ShowNote(int i, bool show) {
    if (i < 0 || i >= kComboNoteCount || !g_notes[i].tip || !g_notes[i].combo)
        return;
    if (show && g_note_up >= 0 && g_note_up != i)
        ShowNote(g_note_up, false);
    TOOLINFOA ti = NoteTool(i, GetParent(g_notes[i].combo));
    SendMessageA(g_notes[i].tip, TTM_TRACKACTIVATE, show ? TRUE : FALSE, (LPARAM)&ti);
    if (show)
        g_note_up = i;
    else if (g_note_up == i)
        g_note_up = -1;
    if (show) {
        // Measure the shown tip (its size is only real once it is up),
        // then park it just above the combo.
        RECT combo, tip;
        GetWindowRect(g_notes[i].combo, &combo);
        GetWindowRect(g_notes[i].tip, &tip);
        SendMessageA(g_notes[i].tip, TTM_TRACKPOSITION, 0,
                     MAKELPARAM(combo.left, combo.top - (tip.bottom - tip.top) - 4));
    }
}
constexpr int B_ROW_H = 26, B_LABEL_W = 96, B_COMBO_W = 300, B_DESC_W = 430;

void FillChoices(HWND combo) {
    SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)"none");
    const char* mods[] = {"", "left_grip+", "right_grip+", "left_grip+right_grip+"};
    for (int m = 0; m < 4; m++) {
        for (int i = 0; i < kInputCount; i++) {
            const InputToken& in = kInputs[i];
            if (IsGrip(in.button) && m != 0) continue;
            if (IsStickFlick(in.button) && m == 0) continue;
            char text[96];
            snprintf(text, sizeof(text), "%s%s", mods[m], in.name);
            SendMessageA(combo, CB_ADDSTRING, 0, (LPARAM)text);
        }
    }
}

int ReadBindingsSection(char* buf, DWORD cap, const char** lines, int max_lines) {
    const DWORD n = GetPrivateProfileSectionA("bindings", buf, cap, g_ini_path);
    int nlines = 0;
    for (const char* p = buf; n > 0 && *p != '\0' && nlines < max_lines; p += strlen(p) + 1)
        lines[nlines++] = p;
    return nlines;
}

void ShowTable(const Table& t) {
    for (int a = 0; a < kActionCount; a++) {
        char chords[256];
        ActionChordsText(t, a, chords, sizeof(chords));
        SetWindowTextA(g_combo[a], chords);
    }
}

void CollectLines(char (*storage)[320], const char** lines, int& nlines) {
    nlines = 0;
    for (int a = 0; a < kActionCount; a++) {
        char value[256];
        GetWindowTextA(g_combo[a], value, sizeof(value));
        snprintf(storage[a], sizeof(storage[a]), "%s=%s", kActions[a].name, value);
        lines[nlines++] = storage[a];
    }
}

void BindingsLoad(bool announce) {
    ClearLog();
    static char buf[16384];
    const char* lines[128];
    const int nlines = ReadBindingsSection(buf, sizeof(buf), lines, 128);
    Table t;
    Build(lines, nlines, t, LogCb, nullptr);
    ShowTable(t);
    char status[512];
    snprintf(status, sizeof(status), "%s  -  %d [bindings] lines: %d accepted, %d rejected",
             g_ini_path, nlines, t.accepted_lines, t.rejected_lines);
    SetStatus(status);
    if (announce)
        AppendLog(nlines == 0 ? "No [bindings] section in the ini: showing the built-in defaults."
                              : "Loaded the ini's [bindings] section (effective table shown).");
}

bool BindingsCheck(Table& t) {
    ClearLog();
    char storage[kActionCount][320];
    const char* lines[kActionCount];
    int nlines = 0;
    CollectLines(storage, lines, nlines);
    Build(lines, nlines, t, LogCb, nullptr);
    int bound = 0;
    for (int a = 0; a < kActionCount; a++)
        for (int i = 0; i < t.count; i++)
            if (t.b[i].action == a) { bound++; break; }
    char line[160];
    snprintf(line, sizeof(line), "Check: %d of %d actions bound, %d line(s) rejected.", bound,
             kActionCount, t.rejected_lines);
    AppendLog(line);
    return t.rejected_lines == 0;
}

void BindingsSave() {
    Table t;
    BindingsCheck(t);
    // Write the EFFECTIVE table, one line per action; lines equal to the
    // default are removed so the ini stays minimal.
    int written = 0, removed = 0;
    for (int a = 0; a < kActionCount; a++) {
        char chords[256];
        ActionChordsText(t, a, chords, sizeof(chords));
        Table d;
        Build(nullptr, 0, d, nullptr, nullptr);
        char def[256];
        ActionChordsText(d, a, def, sizeof(def));
        if (_stricmp(chords, def) == 0) {
            if (WritePrivateProfileStringA("bindings", kActions[a].name, nullptr, g_ini_path))
                removed++;
        } else if (WritePrivateProfileStringA("bindings", kActions[a].name, chords, g_ini_path)) {
            written++;
        } else {
            char msg[400];
            snprintf(msg, sizeof(msg), "Could not write %s (error %lu). Is the file read-only?",
                     g_ini_path, GetLastError());
            MessageBoxA(g_main, msg, "PSOBB VR Options", MB_ICONERROR);
            return;
        }
    }
    ShowTable(t);
    char line[200];
    snprintf(line, sizeof(line), "Saved: %d custom line(s) written (%d action(s) at their default "
             "are left out). A running game picks this up within about a second.", written, removed);
    AppendLog(line);
    SetStatus(line);
}

void BindingsDefaults() {
    Table t;
    Build(nullptr, 0, t, nullptr, nullptr);
    ShowTable(t);
    ClearLog();
    AppendLog("Defaults restored in the editor (the built-in layout) - not saved yet.");
}

// Export / import: an update zip carries a full psobbvr.ini, so unzipping
// it loses custom bindings. Export writes every action (defaults included)
// under a [bindings] header; import runs it through the ini parser into
// the editor and leaves the write to Save.
void IniFolder(char* out, size_t cap) {
    snprintf(out, cap, "%s", g_ini_path);
    char* slash = strrchr(out, '\\');
    if (slash != nullptr) *slash = '\0'; else out[0] = '\0';
}

bool PickBindingsFile(bool save, char* path, size_t cap) {
    char folder[MAX_PATH];
    IniFolder(folder, sizeof(folder));
    snprintf(path, cap, "%s", save ? "psobbvr-bindings.txt" : "");
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = "Bindings files (*.txt)\0*.txt\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = (DWORD)cap;
    ofn.lpstrInitialDir = folder[0] ? folder : nullptr;
    ofn.lpstrDefExt = "txt";
    ofn.lpstrTitle = save ? "Export controller bindings" : "Import controller bindings";
    ofn.Flags = OFN_EXPLORER | OFN_HIDEREADONLY | OFN_NOCHANGEDIR |
                (save ? OFN_OVERWRITEPROMPT : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST));
    return save ? GetSaveFileNameA(&ofn) != FALSE : GetOpenFileNameA(&ofn) != FALSE;
}

void BindingsExport() {
    char path[MAX_PATH];
    if (!PickBindingsFile(true, path, sizeof(path)))
        return;
    char storage[kActionCount][320];
    const char* lines[kActionCount];
    int nlines = 0;
    CollectLines(storage, lines, nlines);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || f == nullptr) {
        char msg[400];
        snprintf(msg, sizeof(msg), "Could not write %s.", path);
        MessageBoxA(g_main, msg, "PSOBB VR Options", MB_ICONERROR);
        return;
    }
    fputs("; PSOBB VR controller bindings - exported by psobbvr_options.exe\r\n"
          "; Every action is listed, so importing restores this layout exactly.\r\n"
          "; To restore after an update: Options program > Controller bindings >\r\n"
          "; Import bindings..., pick this file, then Save.\r\n"
          "[bindings]\r\n", f);
    for (int i = 0; i < nlines; i++) {
        fputs(lines[i], f);
        fputs("\r\n", f);
    }
    fclose(f);
    char line[MAX_PATH + 80];
    snprintf(line, sizeof(line), "Exported %d binding line(s) to %s", nlines, path);
    AppendLog(line);
    SetStatus(line);
}

void BindingsImport() {
    char path[MAX_PATH];
    if (!PickBindingsFile(false, path, sizeof(path)))
        return;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || f == nullptr) {
        char msg[400];
        snprintf(msg, sizeof(msg), "Could not read %s.", path);
        MessageBoxA(g_main, msg, "PSOBB VR Options", MB_ICONERROR);
        return;
    }
    static char buf[65536];
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    // Lines: skip blanks, comments and section headers; keep name=value.
    const char* lines[256];
    int nlines = 0;
    for (char* p = buf; *p != '\0' && nlines < 256;) {
        char* e = strpbrk(p, "\r\n");
        if (e != nullptr) *e = '\0';
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '\0' && *p != ';' && *p != '#' && *p != '[' && strchr(p, '=') != nullptr)
            lines[nlines++] = p;
        if (e == nullptr) break;
        p = e + 1;
    }
    ClearLog();
    Table t;
    Build(lines, nlines, t, LogCb, nullptr);
    ShowTable(t);
    char line[MAX_PATH + 120];
    snprintf(line, sizeof(line), "Imported %s: %d line(s), %d accepted, %d rejected - shown in the "
             "editor, not saved yet. Press Save to write the ini.", path, nlines,
             t.accepted_lines, t.rejected_lines);
    AppendLog(line);
    SetStatus(line);
}

int BuildBindingsPage(HWND page) {
    int y = MARGIN;
    MakeLabel(page, "Which button (or grip + button chord) performs which in-game action. "
                    "Type a chord or pick one; several chords per action are allowed, comma-separated. "
                    "Check shows what the mod will accept; Save writes psobbvr.ini and the running game reloads it.",
              MARGIN, y, PAGE_W - 2 * MARGIN, 44, g_font);
    y += 50;
    MakeLabel(page, "Action", MARGIN, y, B_LABEL_W, 18, g_font_bold);
    MakeLabel(page, "Chord(s)", MARGIN + B_LABEL_W, y, B_COMBO_W, 18, g_font_bold);
    MakeLabel(page, "What it does", MARGIN + B_LABEL_W + B_COMBO_W + 8, y, B_DESC_W, 18, g_font_bold);
    y += 22;
    for (int a = 0; a < kActionCount; a++) {
        MakeLabel(page, kActions[a].name, MARGIN, y + 4, B_LABEL_W, 18, g_font);
        HWND c = CreateWindowExA(0, "COMBOBOX", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                 CBS_DROPDOWN | CBS_AUTOHSCROLL, S(MARGIN + B_LABEL_W), S(y), S(B_COMBO_W), S(400),
                                 page, (HMENU)(INT_PTR)(ID_FIRST_COMBO + a), nullptr, nullptr);
        SendMessageA(c, WM_SETFONT, (WPARAM)g_font, TRUE);
        FillChoices(c);
        g_combo[a] = c;
        for (int n = 0; n < kComboNoteCount; n++) {
            if (strcmp(kActions[a].name, kComboNotes[n].action) != 0)
                continue;
            g_notes[n].combo = c;
            g_notes[n].text = kComboNotes[n].text;
            g_notes[n].tip = CreateWindowExA(WS_EX_TOPMOST, TOOLTIPS_CLASSA, nullptr,
                                             WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                             CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                             CW_USEDEFAULT, page, nullptr, nullptr, nullptr);
            SendMessageA(g_notes[n].tip, TTM_SETMAXTIPWIDTH, 0, S(360));
            TOOLINFOA ti = NoteTool(n, page);
            SendMessageA(g_notes[n].tip, TTM_ADDTOOLA, 0, (LPARAM)&ti);
        }
        char desc[200];
        snprintf(desc, sizeof(desc), "%s   (default: %s)", kActions[a].label,
                 kActions[a].default_chords[0] ? kActions[a].default_chords : "none");
        MakeLabel(page, desc, MARGIN + B_LABEL_W + B_COMBO_W + 8, y + 4, B_DESC_W, 18, g_font);
        y += B_ROW_H;
    }
    y += 6;
    MakeButton(page, "Export bindings...", ID_EXPORT, MARGIN, y, 150, 26);
    MakeButton(page, "Import bindings...", ID_IMPORT, MARGIN + 158, y, 150, 26);
    MakeLabel(page, "Before updating: Export. After updating: Import, then Save.",
              MARGIN + 316, y + 5, PAGE_W - MARGIN - 316 - MARGIN, 18, g_font);
    y += 30;
    return y + MARGIN;
}

// ---------------------------------------------------- page 2: game options

// GRAPHICCTRL = 9 dwords, SOUNDCTRL = 3 dwords.
enum { G_TIER = 0, G_ADVANCED = 1, G_SHADOW = 2, G_ENEMY = 3, G_MAP = 4, G_CLIP = 5, G_FOG = 6,
       G_LOWRES = 7, G_FRAMESKIP = 8 };
// The three presets as option.exe writes them (Normal = the stock
// install.reg blob). They set dwords 0..7 and leave frame skip (dword 8).
constexpr DWORD kGraphicsHighEnd[9] = {0, 1, 2, 1, 2, 2, 1, 0, 0};
constexpr DWORD kGraphicsNormal[9] = {1, 0, 0, 0, 1, 1, 1, 0, 0};
constexpr DWORD kGraphicsLowEnd[9] = {2, 0, 0, 0, 0, 0, 1, 0, 0};
// The mod's fresh-key default, as in common/registry_seed.h: custom tier,
// advanced off, shadow Low, enemy High, map Medium, clip Far (2, the stock
// maximum), vertex fog, low-res off, frame skip 0.
constexpr DWORD kGraphicsVrDefault[9] = {3, 0, 0, 1, 1, 2, 0, 0, 0};
constexpr DWORD kSoundStock[3] = {1, 1, 1};
enum { PRESET_HIGH = 0, PRESET_NORMAL = 1, PRESET_LOW = 2, PRESET_CUSTOM = 3 };

struct GameOpts {
    DWORD g[9];
    DWORD s[3];
    DWORD account_check;
    bool found_key;
};
GameOpts g_game_loaded = {};

HWND g_g_preset, g_g_shadow, g_g_enemy, g_g_map, g_g_clip, g_g_fog, g_g_advanced, g_g_lowres,
     g_g_frameskip, g_g_autofs, g_g_bgm, g_g_se, g_g_saveaccount;

const char* GameKeyPath() {
    // Follow the ini: redirect=1 (default) means the mod's own key.
    return GetPrivateProfileIntA("registry", "redirect", 1, g_ini_path) != 0
               ? "Software\\psobbvr\\PSOBB" : "Software\\SonicTeam\\PSOBB";
}

void GameDefaults(GameOpts& o) {
    memcpy(o.g, kGraphicsVrDefault, sizeof(o.g));
    memcpy(o.s, kSoundStock, sizeof(o.s));
    o.account_check = 0;
}

bool GameRead(GameOpts& o) {
    GameDefaults(o);
    o.found_key = false;
    HKEY key = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, GameKeyPath(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    o.found_key = true;
    BYTE buf[64];
    DWORD size = sizeof(buf), type = 0;
    if (RegQueryValueExA(key, "GRAPHICCTRL", nullptr, &type, buf, &size) == ERROR_SUCCESS &&
        type == REG_BINARY && size == 36)
        memcpy(o.g, buf, 36);
    size = sizeof(buf);
    if (RegQueryValueExA(key, "SOUNDCTRL", nullptr, &type, buf, &size) == ERROR_SUCCESS &&
        type == REG_BINARY && size == 12)
        memcpy(o.s, buf, 12);
    DWORD v = 0;
    size = sizeof(v);
    if (RegQueryValueExA(key, "ACCOUNT_CHECK", nullptr, &type, (BYTE*)&v, &size) == ERROR_SUCCESS &&
        type == REG_DWORD)
        o.account_check = v;
    RegCloseKey(key);
    return true;
}

bool GameWrite(const GameOpts& o) {
    HKEY key = nullptr;
    LSTATUS st = RegCreateKeyExA(HKEY_CURRENT_USER, GameKeyPath(), 0, nullptr, 0,
                                 KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (st != ERROR_SUCCESS) return false;
    // A key this program created before the game's first launch must carry
    // the full stock seed: the game will not start without CTRLBUF.
    regseed::SeedMissingStockValues(key);
    bool ok = true;
    ok &= RegSetValueExA(key, "GRAPHICCTRL", 0, REG_BINARY, (const BYTE*)o.g, 36) == ERROR_SUCCESS;
    ok &= RegSetValueExA(key, "SOUNDCTRL", 0, REG_BINARY, (const BYTE*)o.s, 12) == ERROR_SUCCESS;
    ok &= RegSetValueExA(key, "ACCOUNT_CHECK", 0, REG_DWORD, (const BYTE*)&o.account_check, 4) ==
          ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

// Preset fields equal? Compares dwords 2..7: not the tier, not the
// frame-skip pair, and not "Advanced Effect" (dword 1), which the mod
// forces off at every launch (its screen-space particles make the HUD
// scale up huge in VR), so it can never distinguish presets.
bool FieldsEqual(const DWORD* a, const DWORD* b) {
    for (int i = 2; i < 8; i++)
        if (a[i] != b[i]) return false;
    return true;
}

int PresetOf(const DWORD* g) {
    if (FieldsEqual(g, kGraphicsHighEnd)) return PRESET_HIGH;
    if (FieldsEqual(g, kGraphicsNormal)) return PRESET_NORMAL;
    if (FieldsEqual(g, kGraphicsLowEnd)) return PRESET_LOW;
    return PRESET_CUSTOM;
}

void GameShow(const GameOpts& o) {
    SetSel(g_g_shadow, (int)(o.g[G_SHADOW] > 2 ? 2 : o.g[G_SHADOW]));
    SetSel(g_g_enemy, (int)(o.g[G_ENEMY] > 1 ? 1 : o.g[G_ENEMY]));
    SetSel(g_g_map, (int)(o.g[G_MAP] > 2 ? 2 : o.g[G_MAP]));
    // Clip distance: the game's loader clamps this dword to 0..2 on read
    // (other launchers write 3..6), so anything above 2 is Far - shown as
    // such, and Save normalises it.
    SetSel(g_g_clip, (int)(o.g[G_CLIP] > 2 ? 2 : o.g[G_CLIP]));
    SetSel(g_g_fog, (int)(o.g[G_FOG] > 2 ? 2 : o.g[G_FOG]));
    SetChecked(g_g_advanced, false);   // always shown off: the mod forces it off at launch
    SetChecked(g_g_lowres, o.g[G_LOWRES] != 0);
    const DWORD fs = o.g[G_FRAMESKIP] & 0xFFFF;
    SetSel(g_g_frameskip, (int)(fs > 2 ? 2 : fs));
    SetChecked(g_g_autofs, (o.g[G_FRAMESKIP] >> 16) != 0);
    SetChecked(g_g_bgm, o.s[1] != 0);
    SetChecked(g_g_se, o.s[2] != 0);
    SetChecked(g_g_saveaccount, o.account_check != 0);
    SetSel(g_g_preset, PresetOf(o.g));
}

void GameCollect(GameOpts& o) {
    o = g_game_loaded;
    o.g[G_SHADOW] = (DWORD)Sel(g_g_shadow);
    o.g[G_ENEMY] = (DWORD)Sel(g_g_enemy);
    o.g[G_MAP] = (DWORD)Sel(g_g_map);
    o.g[G_CLIP] = (DWORD)Sel(g_g_clip);
    o.g[G_FOG] = (DWORD)Sel(g_g_fog);
    o.g[G_ADVANCED] = 0;   // forced off by the mod (see FieldsEqual); the box is disabled
    o.g[G_LOWRES] = Checked(g_g_lowres) ? 1 : 0;
    o.g[G_FRAMESKIP] = ((DWORD)Sel(g_g_frameskip) & 0xFFFF) | (Checked(g_g_autofs) ? 0xFFFF0000u : 0);
    // Tier dword as the stock dialog derives it: the preset the fields
    // match, 3 (Custom) otherwise.
    o.g[G_TIER] = (DWORD)PresetOf(o.g);
    o.s[1] = Checked(g_g_bgm) ? 1 : 0;
    o.s[2] = Checked(g_g_se) ? 1 : 0;
    o.s[0] = (o.s[1] && o.s[2]) ? 1 : (!o.s[1] && !o.s[2]) ? 0 : 2;
    o.account_check = Checked(g_g_saveaccount) ? 1 : 0;
}

void GameLoad(bool announce) {
    ClearLog();
    GameRead(g_game_loaded);
    GameShow(g_game_loaded);
    char status[400];
    snprintf(status, sizeof(status), "Registry key HKCU\\%s  -  %s", GameKeyPath(),
             g_game_loaded.found_key ? "loaded" : "not created yet (showing the stock defaults; "
                                                  "Save creates it, and so does the first game launch)");
    SetStatus(status);
    if (announce) AppendLog(status);
}

void GameSave() {
    ClearLog();
    GameOpts o;
    GameCollect(o);
    if (!GameWrite(o)) {
        char msg[300];
        snprintf(msg, sizeof(msg), "Could not write HKCU\\%s (error %lu).", GameKeyPath(),
                 GetLastError());
        MessageBoxA(g_main, msg, "PSOBB VR Options", MB_ICONERROR);
        return;
    }
    g_game_loaded = o;
    g_game_loaded.found_key = true;
    GameShow(o);
    char line[300];
    snprintf(line, sizeof(line),
             "Saved to HKCU\\%s: graphics tier %lu, shadow %lu (the mod forces Low at launch), "
             "BGM %s, SE %s, save account %s. Takes effect at the next game launch.",
             GameKeyPath(), (unsigned long)o.g[G_TIER], (unsigned long)o.g[G_SHADOW],
             o.s[1] ? "on" : "off", o.s[2] ? "on" : "off", o.account_check ? "on" : "off");
    AppendLog(line);
    SetStatus("Saved. Game options take effect at the next game launch.");
}

void GameDefaultsToUi() {
    GameOpts o = g_game_loaded;
    GameDefaults(o);
    GameShow(o);
    ClearLog();
    AppendLog("The mod's defaults shown (what VR was tuned with: enemy High, map Medium, clip Far, vertex fog; sound on; account not saved) - not saved yet.");
}

void GameApplyPreset(int idx) {
    GameOpts o;
    GameCollect(o);
    const DWORD* p = idx == PRESET_HIGH ? kGraphicsHighEnd
                   : idx == PRESET_NORMAL ? kGraphicsNormal
                   : idx == PRESET_LOW ? kGraphicsLowEnd : nullptr;
    if (p == nullptr) return;   // Custom: leave the fields alone
    memcpy(o.g, p, 8 * sizeof(DWORD));   // dwords 0..7; frame skip stays
    GameShow(o);
}

void GameFieldChanged() {
    // Re-derive the preset drop-down from the fields.
    GameOpts o;
    GameCollect(o);
    SetSel(g_g_preset, PresetOf(o.g));
}

int BuildGamePage(HWND page) {
    int y = MARGIN;
    MakeLabel(page, "The settings the original game's option.exe managed. The mod keeps them in its own "
                    "registry key, so other PSO installs on this PC are never affected. Resolution, colour "
                    "depth, V-Sync and window mode are controlled by the mod and not shown here. Shadow "
                    "detail is forced to Low at every launch (the detailed shadows swim in VR). Changes "
                    "take effect at the next game launch.",
              MARGIN, y, PAGE_W - 2 * MARGIN, 60, g_font);
    y += 66;
    const int LBL = 150, CTL = 180, X0 = MARGIN + 10;
    // Graphics group
    const int gy = y;
    int yy = gy + 22;
    const char* presets[] = {"High End", "Normal (stock)", "Low End", "Custom"};
    MakeLabel(page, "Preset", X0, yy + 3, LBL, 18, g_font);
    g_g_preset = MakeDropList(page, ID_G_PRESET, X0 + LBL, yy, CTL, presets, 4);
    MakeLabel(page, "Fills the fields below, except frame skip",
              X0 + LBL + CTL + 10, yy + 3, 460, 18, g_font);
    yy += 28;
    const char* lmh[] = {"Low", "Medium", "High"};
    const char* lh[] = {"Low", "High"};
    const char* clip[] = {"Near", "Mid", "Far"};   // the game clamps the dword to 0..2 on read
    const char* fog[] = {"Vertex fog", "Pixel fog", "Fog emulation"};
    const char* fs[] = {"0", "1", "2"};
    MakeLabel(page, "Shadow detail", X0, yy + 3, LBL, 18, g_font);
    g_g_shadow = MakeDropList(page, ID_G_SHADOW, X0 + LBL, yy, CTL, lmh, 3);
    MakeLabel(page, "forced to Low at launch by the mod - kept here so presets round-trip",
              X0 + LBL + CTL + 10, yy + 3, 460, 18, g_font);
    yy += 28;
    MakeLabel(page, "Enemy detail", X0, yy + 3, LBL, 18, g_font);
    g_g_enemy = MakeDropList(page, ID_G_ENEMY, X0 + LBL, yy, CTL, lh, 2);
    yy += 28;
    MakeLabel(page, "Map detail", X0, yy + 3, LBL, 18, g_font);
    g_g_map = MakeDropList(page, ID_G_MAP, X0 + LBL, yy, CTL, lmh, 3);
    yy += 28;
    MakeLabel(page, "Clip distance", X0, yy + 3, LBL, 18, g_font);
    g_g_clip = MakeDropList(page, ID_G_CLIP, X0 + LBL, yy, CTL, clip, 3);
    MakeLabel(page, "the game's own draw distance; the mod multiplies it further (VR page)",
              X0 + LBL + CTL + 10, yy + 3, 460, 18, g_font);
    yy += 28;
    MakeLabel(page, "Fog effect", X0, yy + 3, LBL, 18, g_font);
    g_g_fog = MakeDropList(page, ID_G_FOG, X0 + LBL, yy, CTL, fog, 3);
    yy += 28;
    MakeLabel(page, "Frame skip", X0, yy + 3, LBL, 18, g_font);
    g_g_frameskip = MakeDropList(page, ID_G_FRAMESKIP, X0 + LBL, yy, CTL, fs, 3);
    MakeLabel(page, "leave at 0 in VR", X0 + LBL + CTL + 10, yy + 3, 460, 18, g_font);
    yy += 28;
    g_g_advanced = MakeCheck(page, "Advanced effects (forced off)", ID_G_ADVANCED, X0, yy, 220);
    EnableWindow(g_g_advanced, FALSE);
    g_g_lowres = MakeCheck(page, "Low resolution textures", ID_G_LOWRES, X0 + 210, yy, 200);
    g_g_autofs = MakeCheck(page, "Auto frame skip", ID_G_AUTOFS, X0 + 420, yy, 200);
    yy += 24;
    MakeLabel(page, "Advanced effects stay off: their screen-space particles make the HUD jump to huge in VR.",
              X0, yy, PAGE_W - 2 * MARGIN - 20, 18, g_font);
    yy += 26;
    MakeGroup(page, "Graphics", MARGIN, gy, PAGE_W - 2 * MARGIN, yy - gy);
    y = yy + 10;
    // Sound + login groups side by side
    const int sy = y;
    yy = sy + 22;
    g_g_bgm = MakeCheck(page, "Background music (BGM)", ID_G_BGM, X0, yy, 200);
    yy += 24;
    g_g_se = MakeCheck(page, "Sound effects (SE)", ID_G_SE, X0, yy, 200);
    yy += 30;
    MakeGroup(page, "Sound", MARGIN, sy, 400, yy - sy);
    g_g_saveaccount = MakeCheck(page, "Save ID and password at the login screen", ID_G_SAVEACCOUNT,
                                MARGIN + 420 + 10, sy + 22, 380);
    MakeGroup(page, "Login", MARGIN + 420, sy, PAGE_W - 2 * MARGIN - 420, yy - sy);
    y = yy + 10;
    return y + MARGIN;
}

// ------------------------------------------------------ page 3: VR knobs

enum KnobType { K_FLOAT, K_INT, K_BOOL, K_CHOICE, K_RESOLUTION };

struct Knob {
    const char* section;
    const char* key;
    const char* label;
    KnobType type;
    double min, max;
    const char* def;          // default as ini text
    const char* choices;      // K_CHOICE: "label=value|label=value"
    const char* desc;         // shown in the status line while focused
    const char* mirror_key;   // a second key saved with the same value
};

// The settings shown; everything else stays ini-only. The mod reads them
// at launch.
const Knob kKnobs[] = {
    // --- Comfort and view
    {"render", "width", "Resolution per eye", K_RESOLUTION, 0, 0, "2880x2160",
     "1920x1440=1920x1440|2304x1728=2304x1728|2560x1920=2560x1920|2880x2160=2880x2160|3200x2400=3200x2400",
     "The image size rendered for each eye. Lower it if the headset stutters."},
    {"vr", "world_scale", "World scale", K_FLOAT, 5, 15, "9", nullptr,
     "How big the world feels. 9 makes characters their true height; higher makes the world smaller."},
    {"vr", "eye_offset_m", "Eye height offset (m)", K_FLOAT, 0, 0.8, "0.36", nullptr,
     "Raises your viewpoint above your character's head. Raise it if you see from inside your chest."},
    {"vr", "text_scale", "Damage number size", K_FLOAT, 0.2, 2, "0.5", nullptr,
     "Size of the damage numbers. 1 = the original game's size."},
    {"vr", "hud_distance_m", "HUD distance (m)", K_FLOAT, 0.8, 4, "1.5", nullptr,
     "How far away the HUD floats."},
    {"vr", "hud_width_deg", "HUD width (degrees)", K_INT, 30, 110, "60", nullptr,
     "How much of your view the HUD covers, side to side."},
    {"vr", "hud_lock", "HUD position", K_CHOICE, 0, 0, "1",
     "In front of your character=1|Fixed in the room=2|Follows your head=0",
     "In front of your character: stays put while you look around and turns when you turn. "
     "Fixed in the room: stays where you faced at your last recenter. Follows your head: always in view."},
    {"vr", "vr_keyboard", "On-screen keyboard", K_BOOL, 0, 1, "1", nullptr,
     "Shows a keyboard whenever the game asks for text (chat, login, names). Point at a key and pull the trigger."},
    {"vr", "menu_distance_m", "Menu distance (m)", K_FLOAT, 1, 5, "2.0", nullptr,
     "How far away menus float."},
    {"vr", "menu_width_deg", "Menu width (degrees)", K_INT, 30, 120, "70", nullptr,
     "How much of your view menus cover, side to side."},
    {"vr", "draw_distance_scale", "Terrain draw distance", K_FLOAT, 0.25, 16, "4", nullptr,
     "Multiplies how far the level's terrain is drawn. Enemies, NPCs, items and boxes keep the game's own "
     "limits. Higher values cost performance in town."},
    // --- Movement
    {"vr", "stick_turn_deg_s", "Turn speed (degrees/s)", K_INT, 30, 360, "140", nullptr,
     "How fast the right stick turns you when pushed all the way."},
    {"vr", "controller_deadzone", "Stick deadzone", K_FLOAT, 0, 0.6, "0.15", nullptr,
     "How far a stick must move before it counts. Raise it if your character drifts on its own."},
    {"vr", "controller_press", "Trigger/grip press point", K_FLOAT, 0.2, 0.95, "0.6", nullptr,
     "How far a trigger or grip must be pulled to count as a press."},
    {"vr", "head_move", "Head-based locomotion", K_BOOL, 0, 2, "0", nullptr,
     "On: pushing forward walks where your head points, and your character turns to match. "
     "Off: pushing forward walks where your character faces."},
    // --- Hands and combat
    {"vr", "hand_presence", "Show hands", K_BOOL, 0, 1, "1", nullptr,
     "Draws your character's hands on the controllers."},
    {"vr", "weapon_scale", "Hand and weapon size", K_FLOAT, 0.1, 2, "0.7", nullptr,
     "Size of your hands and held weapon. 1 = the game's original (oversized) models.", "hand_scale"},
    {"vr", "hand_pitch_deg", "Hand angle (degrees)", K_FLOAT, -45, 45, "-10", nullptr,
     "Tilts the hands on the controllers. Negative points the fingers down."},
    {"vr", "swing_attack", "Swing to attack", K_BOOL, 0, 2, "2", nullptr,
     "On: swing to attack. For heavy and special attacks, press their button, then swing. "
     "Off: attacks fire on the button press."},
    {"vr", "cast_swing", "Swing to cast", K_BOOL, 0, 1, "1", nullptr,
     "On: the button picks the technique and a swing casts it. Off: techniques cast on the button press."},
    {"vr", "cast_left_hand", "Left handed casting", K_BOOL, 0, 1, "1", nullptr,
     "On: support techniques and ones that find their own target (Resta, Shifta, Zonde...) cast from either "
     "hand's swing. Aimed techniques (Foie, Barta, Megid...) always cast from the right."},
    {"vr", "swing_indicator", "Swing timing indicator", K_BOOL, 0, 1, "1", nullptr,
     "A hexagon at the top of your view: green = a swing starts an attack, blue = a swing lands the next hit, "
     "grey = wait. The shrinking ring shows when it is due."},
    {"vr", "attack_retarget", "Head-based combo targeting", K_BOOL, 0, 1, "1", nullptr,
     "On: each hit of a combo goes at the enemy you are looking at. Off: the whole combo stays on the first "
     "hit's enemy, as in the original game."},
    {"vr", "gun_haptic", "Gun vibration", K_CHOICE, 0, 0, "2", "Off=0|Per trigger pull=1|Per shot=2",
     "Controller vibration when you fire a gun."},
    {"vr", "hit_haptic", "Melee hit vibration", K_BOOL, 0, 1, "1", nullptr,
     "Vibrates your weapon hand when a melee hit lands."},
    {"vr", "hurt_haptic", "Damage vibration", K_BOOL, 0, 1, "1", nullptr,
     "Vibrates both controllers when you take damage."},
    // --- Troubleshooting
    {"debug", "enabled", "Diagnostic logging", K_BOOL, 0, 1, "1", nullptr,
     "Writes psobbvr-vr.log in the game folder (the last three launches are kept). Send it with bug reports."},
};
constexpr int kKnobCount = sizeof(kKnobs) / sizeof(kKnobs[0]);
// First knob of each group. The first two groups share the left column.
constexpr int kGroupStarts[] = {0, 11, 15, 26};
const char* kGroupNames[] = {"Comfort and view", "Movement", "Hands and combat", "Troubleshooting"};
constexpr int kGroupCount = sizeof(kGroupStarts) / sizeof(kGroupStarts[0]);
HWND g_knob[kKnobCount] = {};

// K_CHOICE helpers: parse "label=value|label=value".
int ChoiceCount(const Knob& k) {
    int n = 1;
    for (const char* p = k.choices; *p; p++) if (*p == '|') n++;
    return n;
}
void ChoiceAt(const Knob& k, int idx, char* label, size_t lcap, char* value, size_t vcap) {
    const char* p = k.choices;
    for (int i = 0; i < idx; i++) { p = strchr(p, '|'); p = p ? p + 1 : k.choices; }
    const char* eq = strchr(p, '=');
    const char* end = strchr(p, '|');
    if (!end) end = p + strlen(p);
    size_t ll = (size_t)(eq - p); if (ll >= lcap) ll = lcap - 1;
    memcpy(label, p, ll); label[ll] = 0;
    size_t vl = (size_t)(end - eq - 1); if (vl >= vcap) vl = vcap - 1;
    memcpy(value, eq + 1, vl); value[vl] = 0;
}
int ChoiceIndexOf(const Knob& k, const char* value) {
    const int n = ChoiceCount(k);
    for (int i = 0; i < n; i++) {
        char l[64], v[64];
        ChoiceAt(k, i, l, sizeof(l), v, sizeof(v));
        if (_stricmp(v, value) == 0) return i;
    }
    return -1;
}

void ReadIniValue(const Knob& k, char* out, size_t cap) {
    if (k.type == K_RESOLUTION) {
        char w[32], h[32];
        GetPrivateProfileStringA("render", "width", "2880", w, sizeof(w), g_ini_path);
        GetPrivateProfileStringA("render", "height", "2160", h, sizeof(h), g_ini_path);
        snprintf(out, cap, "%sx%s", w, h);
        return;
    }
    GetPrivateProfileStringA(k.section, k.key, k.def, out, (DWORD)cap, g_ini_path);
    // trim
    char* e = out + strlen(out);
    while (e > out && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
}

void KnobShow(int i, const char* value) {
    const Knob& k = kKnobs[i];
    switch (k.type) {
    case K_BOOL: SetChecked(g_knob[i], atoi(value) != 0); break;
    case K_CHOICE:
    case K_RESOLUTION: {
        int idx = ChoiceIndexOf(k, value);
        if (idx < 0) {
            // Unknown value (hand-edited ini): add it so nothing is lost.
            char text[64];
            snprintf(text, sizeof(text), "%s (custom)", value);
            idx = (int)SendMessageA(g_knob[i], CB_ADDSTRING, 0, (LPARAM)text);
        }
        SetSel(g_knob[i], idx);
        break;
    }
    default: SetWindowTextA(g_knob[i], value); break;
    }
}

// Value the control holds, as ini text. False = out of range / not a number.
bool KnobCollect(int i, char* out, size_t cap, char* err, size_t ecap) {
    const Knob& k = kKnobs[i];
    err[0] = 0;
    switch (k.type) {
    // Checked writes the knob's max (1 for plain on/off; swing_attack's "on" is mode 2).
    case K_BOOL: snprintf(out, cap, "%d", Checked(g_knob[i]) ? (int)k.max : 0); return true;
    case K_CHOICE:
    case K_RESOLUTION: {
        const int idx = Sel(g_knob[i]);
        if (idx < 0) { snprintf(err, ecap, "%s: nothing selected", k.label); return false; }
        if (idx < ChoiceCount(k)) {
            char l[64];
            ChoiceAt(k, idx, l, sizeof(l), out, cap);
        } else {
            // the "(custom)" entry: take the text before the space
            char text[64];
            SendMessageA(g_knob[i], CB_GETLBTEXT, idx, (LPARAM)text);
            char* sp = strchr(text, ' ');
            if (sp) *sp = 0;
            snprintf(out, cap, "%s", text);
        }
        return true;
    }
    default: {
        char text[64];
        GetWindowTextA(g_knob[i], text, sizeof(text));
        char* endp = nullptr;
        const double v = strtod(text, &endp);
        if (endp == text || *endp != 0) {
            snprintf(err, ecap, "%s: '%s' is not a number", k.label, text);
            return false;
        }
        if (v < k.min || v > k.max) {
            snprintf(err, ecap, "%s: %s is outside %g..%g", k.label, text, k.min, k.max);
            return false;
        }
        if (k.type == K_INT) snprintf(out, cap, "%d", (int)(v + (v < 0 ? -0.5 : 0.5)));
        else snprintf(out, cap, "%s", text);
        return true;
    }
    }
}

void VrLoad(bool announce) {
    ClearLog();
    for (int i = 0; i < kKnobCount; i++) {
        char v[64];
        ReadIniValue(kKnobs[i], v, sizeof(v));
        KnobShow(i, v);
    }
    char status[400];
    snprintf(status, sizeof(status), "%s  -  VR settings loaded.", g_ini_path);
    SetStatus(status);
    if (announce) AppendLog("VR settings loaded from psobbvr.ini. Click a field to see what it does.");
}

void VrSave() {
    ClearLog();
    // Validate everything first; nothing is written if any field is bad.
    char values[kKnobCount][64];
    bool ok = true;
    for (int i = 0; i < kKnobCount; i++) {
        char err[200];
        if (!KnobCollect(i, values[i], sizeof(values[i]), err, sizeof(err))) {
            AppendLog(err);
            ok = false;
        }
    }
    if (!ok) {
        AppendLog("Nothing saved - fix the value(s) above first.");
        SetStatus("Not saved: a value is out of range.");
        return;
    }
    int written = 0;
    for (int i = 0; i < kKnobCount; i++) {
        const Knob& k = kKnobs[i];
        char cur[64];
        ReadIniValue(k, cur, sizeof(cur));
        char mirror_cur[64] = "";
        if (k.mirror_key)
            GetPrivateProfileStringA(k.section, k.mirror_key, k.def, mirror_cur, sizeof(mirror_cur), g_ini_path);
        // Unchanged: leave the line (and its comments) alone.
        if (_stricmp(cur, values[i]) == 0 && (!k.mirror_key || _stricmp(mirror_cur, values[i]) == 0))
            continue;
        bool w;
        if (k.type == K_RESOLUTION) {
            char* x = strchr(values[i], 'x');
            if (!x) continue;
            *x = 0;
            w = WritePrivateProfileStringA("render", "width", values[i], g_ini_path) &&
                WritePrivateProfileStringA("render", "height", x + 1, g_ini_path);
            *x = 'x';
        } else {
            w = WritePrivateProfileStringA(k.section, k.key, values[i], g_ini_path) != 0;
            if (w && k.mirror_key)
                w = WritePrivateProfileStringA(k.section, k.mirror_key, values[i], g_ini_path) != 0;
        }
        if (!w) {
            char msg[400];
            snprintf(msg, sizeof(msg), "Could not write %s (error %lu). Is the file read-only?",
                     g_ini_path, GetLastError());
            MessageBoxA(g_main, msg, "PSOBB VR Options", MB_ICONERROR);
            return;
        }
        char line[200];
        snprintf(line, sizeof(line), "%s: %s -> %s", k.label, cur, values[i]);
        AppendLog(line);
        written++;
    }
    char line[400];
    if (written == 0)
        snprintf(line, sizeof(line), "Nothing changed - psobbvr.ini already holds these values.");
    else
        snprintf(line, sizeof(line), "Saved %d setting(s) to psobbvr.ini. They take effect the next time "
                 "the game starts.", written);
    AppendLog(line);
    SetStatus(line);
}

void VrDefaults() {
    for (int i = 0; i < kKnobCount; i++) KnobShow(i, kKnobs[i].def);
    ClearLog();
    AppendLog("Shipped defaults shown for every VR setting - not saved yet.");
}

int BuildVrPage(HWND page) {
    int y = MARGIN;
    MakeLabel(page, "The mod's settings, saved in psobbvr.ini. Click a setting to see what it does in the "
                    "line at the bottom. Changes take effect the next time the game starts. More settings "
                    "are in the ini itself.",
              MARGIN, y, PAGE_W - 2 * MARGIN, 44, g_font);
    y += 52;
    // Two columns: comfort and movement on the left, hands and combat on
    // the right.
    const int COL_W = (PAGE_W - 2 * MARGIN) / 2;
    const int LBL = 180, CTL = 110, ROW = 27;
    int col_y[2] = {y, y};
    for (int i = 0; i < kKnobCount; i++) {
        const int col = i < kGroupStarts[2] ? 0 : 1;
        const int x0 = MARGIN + col * COL_W;
        for (int g = 0; g < kGroupCount; g++) {
            if (kGroupStarts[g] == i) {
                MakeLabel(page, kGroupNames[g], x0, col_y[col] + 4, COL_W - 10, 18, g_font_bold);
                col_y[col] += 24;
            }
        }
        const Knob& k = kKnobs[i];
        const int yy = col_y[col];
        const int id = ID_VR_FIRST + i;
        const char* label = k.label;
        switch (k.type) {
        case K_BOOL:
            g_knob[i] = MakeCheck(page, label, id, x0, yy + 1, LBL + CTL);
            break;
        case K_CHOICE:
        case K_RESOLUTION: {
            MakeLabel(page, label, x0, yy + 4, LBL, 18, g_font);
            const int n = ChoiceCount(k);
            g_knob[i] = MakeDropList(page, id, x0 + LBL, yy, COL_W - LBL - 16, nullptr, 0);
            for (int c = 0; c < n; c++) {
                char l[64], v[64];
                ChoiceAt(k, c, l, sizeof(l), v, sizeof(v));
                SendMessageA(g_knob[i], CB_ADDSTRING, 0, (LPARAM)l);
            }
            break;
        }
        default: {
            MakeLabel(page, label, x0, yy + 4, LBL, 18, g_font);
            g_knob[i] = MakeEdit(page, id, x0 + LBL, yy, CTL - 30);
            char rng[64];
            snprintf(rng, sizeof(rng), "%g..%g  (default %s)", k.min, k.max, k.def);
            MakeLabel(page, rng, x0 + LBL + CTL - 24, yy + 4, COL_W - LBL - CTL + 20, 18, g_font);
            break;
        }
        }
        col_y[col] += ROW;
    }
    y = col_y[0] > col_y[1] ? col_y[0] : col_y[1];
    return y + MARGIN;
}

// --------------------------------------------------------- page plumbing

// Children send WM_COMMAND to the page window; forward to the main window.
// A mouse-down anywhere but on the quick_menu combo (its text box, its
// arrow) closes the note - the page and the main window see every
// child's click through WM_PARENTNOTIFY, their own background clicks
// directly. Focus does not move for a click on a label or blank space,
// so the kill-focus path alone would leave the note up.
void NoteOnClick() {
    if (g_note_up < 0)
        return;
    POINT pt;
    GetCursorPos(&pt);
    const HWND hit = WindowFromPoint(pt);
    const HWND combo = g_notes[g_note_up].combo;
    if (hit == combo || (hit && GetParent(hit) == combo))
        return;
    ShowNote(g_note_up, false);
}

bool IsMouseDownNotify(UINT msg, WPARAM wp) {
    if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN ||
        msg == WM_NCLBUTTONDOWN)
        return true;
    if (msg == WM_PARENTNOTIFY) {
        const UINT ev = LOWORD(wp);
        return ev == WM_LBUTTONDOWN || ev == WM_RBUTTONDOWN || ev == WM_MBUTTONDOWN;
    }
    return false;
}

int PageIndexOf(HWND wnd) {
    for (int i = 0; i < PAGE_COUNT; i++) if (g_page[i] == wnd) return i;
    return -1;
}

// Scroll a page to pos (clamped), moving its children with it.
void ScrollPageTo(int i, int pos) {
    const int max_pos = g_page_content[i] - g_page_view;
    if (pos > max_pos) pos = max_pos;
    if (pos < 0) pos = 0;
    const int delta = g_page_scroll[i] - pos;
    if (delta == 0) return;
    g_page_scroll[i] = pos;
    if (g_note_up >= 0) ShowNote(g_note_up, false);
    ScrollWindowEx(g_page[i], 0, delta, nullptr, nullptr, nullptr, nullptr,
                   SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    SetScrollPos(g_page[i], SB_VERT, pos, TRUE);
}

void OnPageScroll(int i, WPARAM wp) {
    const int line = S(26);
    int pos = g_page_scroll[i];
    switch (LOWORD(wp)) {
    case SB_LINEUP: pos -= line; break;
    case SB_LINEDOWN: pos += line; break;
    case SB_PAGEUP: pos -= g_page_view - line; break;
    case SB_PAGEDOWN: pos += g_page_view - line; break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: {
        SCROLLINFO si = {sizeof(si), SIF_TRACKPOS};
        GetScrollInfo(g_page[i], SB_VERT, &si);
        pos = si.nTrackPos;
        break;
    }
    case SB_TOP: pos = 0; break;
    case SB_BOTTOM: pos = g_page_content[i]; break;
    default: return;
    }
    ScrollPageTo(i, pos);
}

LRESULT CALLBACK PageProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (IsMouseDownNotify(msg, wp))
        NoteOnClick();
    switch (msg) {
    case WM_VSCROLL: {
        const int i = PageIndexOf(wnd);
        if (i >= 0) OnPageScroll(i, wp);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int i = PageIndexOf(wnd);
        if (i >= 0 && g_page_content[i] > g_page_view)
            ScrollPageTo(i, g_page_scroll[i] - GET_WHEEL_DELTA_WPARAM(wp) * S(26) * 3 / WHEEL_DELTA);
        return 0;
    }
    case WM_COMMAND: {
        // Combo notes: up while a noted combo has the focus (a click on
        // the text box or the arrow), gone when the focus leaves.
        const int n = NoteIndexOf((HWND)lp);
        if (n >= 0) {
            if (HIWORD(wp) == CBN_SETFOCUS)
                ShowNote(n, true);
            else if (HIWORD(wp) == CBN_KILLFOCUS)
                ShowNote(n, false);
        }
        return SendMessageA(GetParent(wnd), msg, wp, lp);
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    return DefWindowProcA(wnd, msg, wp, lp);
}

// Switching tabs only shows the page - unsaved edits on every page stay
// put until Save or Reload.
void ShowPage(int idx) {
    g_page_index = idx;
    for (int i = 0; i < PAGE_COUNT; i++) ShowWindow(g_page[i], i == idx ? SW_SHOW : SW_HIDE);
    EnableWindow(GetDlgItem(g_main, ID_CHECK), idx == PAGE_BINDINGS);
    EnableWindow(GetDlgItem(g_main, ID_OPENINI), idx != PAGE_GAME);
    switch (idx) {
    case PAGE_BINDINGS:
        SetStatus("Controller bindings - Save writes the ini's [bindings] section; the running game reloads it within a second.");
        break;
    case PAGE_GAME:
        SetStatus("Game options - Save writes the mod's registry key; the game reads it at launch.");
        break;
    case PAGE_VR:
        SetStatus("VR settings - Save writes psobbvr.ini; changes take effect the next time the game starts.");
        break;
    }
}

void LoadAllPages() {
    BindingsLoad(false);
    GameLoad(false);
    VrLoad(false);
    ClearLog();
    AppendLog("Loaded: controller bindings and VR settings from psobbvr.ini, game options from the registry.");
}

// ------------------------------------------------------------ DPI + layout

// Per-monitor DPI helpers, looked up at run time (Windows 10 1607+ for
// the ...ForDpi functions); the fallbacks use the system DPI.
template <typename Fn> Fn User32(const char* name) {
    return reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleA("user32.dll"), name));
}

void EnableDpiAwareness() {
    using SetCtxFn = BOOL(WINAPI*)(HANDLE);
    const auto set_ctx = User32<SetCtxFn>("SetProcessDpiAwarenessContext");
    // (HANDLE)-4 = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2.
    if (set_ctx == nullptr || !set_ctx(reinterpret_cast<HANDLE>(-4)))
        SetProcessDPIAware();
}

int SystemDpi() {
    using Fn = UINT(WINAPI*)();
    if (const auto fn = User32<Fn>("GetDpiForSystem")) return (int)fn();
    HDC dc = GetDC(nullptr);
    const int dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(nullptr, dc);
    return dpi;
}

int WindowDpi(HWND wnd) {
    using Fn = UINT(WINAPI*)(HWND);
    if (const auto fn = User32<Fn>("GetDpiForWindow")) return (int)fn(wnd);
    return SystemDpi();
}

int ScrollBarWidth() {
    using Fn = int(WINAPI*)(int, UINT);
    if (const auto fn = User32<Fn>("GetSystemMetricsForDpi")) return fn(SM_CXVSCROLL, (UINT)g_dpi);
    return MulDiv(GetSystemMetrics(SM_CXVSCROLL), g_dpi, SystemDpi());
}

// Outer window size for a client size, at the window's DPI.
SIZE OuterSize(int client_w, int client_h) {
    RECT r = {0, 0, client_w, client_h};
    const DWORD style = (DWORD)GetWindowLong(g_main, GWL_STYLE);
    using Fn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    if (const auto fn = User32<Fn>("AdjustWindowRectExForDpi")) fn(&r, style, FALSE, 0, (UINT)g_dpi);
    else AdjustWindowRect(&r, style, FALSE);
    return SIZE{r.right - r.left, r.bottom - r.top};
}

// The message font at the current DPI, plus bold and monospace variants.
void MakeFonts() {
    NONCLIENTMETRICSA ncm = {sizeof(ncm)};
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    LOGFONTA lf = ncm.lfMessageFont;
    lf.lfHeight = MulDiv(lf.lfHeight, g_dpi, SystemDpi());
    g_font = CreateFontIndirectA(&lf);
    LOGFONTA bold = lf;
    bold.lfWeight = FW_BOLD;
    g_font_bold = CreateFontIndirectA(&bold);
    LOGFONTA mono = lf;
    strcpy_s(mono.lfFaceName, "Consolas");
    mono.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    g_font_mono = CreateFontIndirectA(&mono);
}

bool g_ui_ready = false;

// Heights around the pages: the tab strip above; buttons, log and
// status line below.
int TopHeight() { return S(MARGIN + TAB_H + 4); }
int BelowHeight() { return S(6 + 34 + LOG_H + 6 + 40); }
int PageWidth() { return S(PAGE_W) + ScrollBarWidth(); }
int TallestPage() {
    int t = 0;
    for (int i = 0; i < PAGE_COUNT; i++) if (g_page_content[i] > t) t = g_page_content[i];
    return t;
}
// Client size that shows every page without scrolling.
SIZE FullClientSize() {
    return SIZE{PageWidth() + 2 * S(MARGIN), TopHeight() + TallestPage() + BelowHeight()};
}

struct ButtonSlot { int id, x, w; };
const ButtonSlot kButtons[] = {
    {ID_CHECK, MARGIN, 90}, {ID_SAVE, MARGIN + 98, 90}, {ID_RELOAD, MARGIN + 196, 100},
    {ID_DEFAULTS, MARGIN + 304, 130}, {ID_OPENINI, MARGIN + 442, 140},
};

// Positions everything for the current client height; a page taller
// than the space between the tabs and the buttons gets a scroll bar.
void Layout() {
    if (!g_ui_ready) return;
    RECT rc;
    GetClientRect(g_main, &rc);
    const int top = TopHeight();
    g_page_view = rc.bottom - top - BelowHeight();
    if (g_page_view < S(80)) g_page_view = S(80);
    const int page_w = PageWidth();
    SetWindowPos(g_tab, nullptr, S(MARGIN), S(MARGIN), page_w, S(TAB_H + 4), SWP_NOZORDER | SWP_NOACTIVATE);
    for (int i = 0; i < PAGE_COUNT; i++) {
        SetWindowPos(g_page[i], nullptr, S(MARGIN), top, page_w, g_page_view, SWP_NOZORDER | SWP_NOACTIVATE);
        ScrollPageTo(i, g_page_scroll[i]);   // re-clamp to the new height
        const bool scrolls = g_page_content[i] > g_page_view;
        if (scrolls) {
            SCROLLINFO si = {sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS};
            si.nMax = g_page_content[i] - 1;
            si.nPage = (UINT)g_page_view;
            si.nPos = g_page_scroll[i];
            SetScrollInfo(g_page[i], SB_VERT, &si, TRUE);
        }
        ShowScrollBar(g_page[i], SB_VERT, scrolls);
    }
    const int y = top + g_page_view + S(6);
    for (const ButtonSlot& b : kButtons)
        SetWindowPos(GetDlgItem(g_main, b.id), nullptr, S(b.x), y, S(b.w), S(26), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g_log, nullptr, S(MARGIN), y + S(34), page_w, S(LOG_H), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(g_status, nullptr, S(MARGIN), y + S(34 + LOG_H + 6), S(PAGE_W), S(36),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g_main, nullptr, TRUE);
}

// Full size if it fits the monitor's work area, else as tall as it
// allows; kept inside the work area.
void FitToWorkArea() {
    const SIZE full = FullClientSize();
    SIZE outer = OuterSize(full.cx, full.cy);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoA(MonitorFromWindow(g_main, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT& work = mi.rcWork;
    if (outer.cy > work.bottom - work.top) outer.cy = work.bottom - work.top;
    if (outer.cx > work.right - work.left) outer.cx = work.right - work.left;
    RECT wr;
    GetWindowRect(g_main, &wr);
    int x = wr.left, y = wr.top;
    if (x + outer.cx > work.right) x = work.right - outer.cx;
    if (y + outer.cy > work.bottom) y = work.bottom - outer.cy;
    if (x < work.left) x = work.left;
    if (y < work.top) y = work.top;
    SetWindowPos(g_main, nullptr, x, y, outer.cx, outer.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    Layout();
}

void BuildUi(HWND wnd) {
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtrA(wnd, GWLP_HINSTANCE);
    WNDCLASSA pc = {};
    pc.lpfnWndProc = PageProc;
    pc.hInstance = inst;
    pc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    pc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    pc.lpszClassName = "PsobbvrOptionsPage";
    RegisterClassA(&pc);

    g_tab = CreateWindowExA(0, WC_TABCONTROLA, "", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
                            0, 0, S(PAGE_W), S(TAB_H + 4), wnd, nullptr, inst, nullptr);
    SendMessageA(g_tab, WM_SETFONT, (WPARAM)g_font, TRUE);
    const char* names[PAGE_COUNT] = {"Controller bindings", "Game options", "VR settings"};
    for (int i = 0; i < PAGE_COUNT; i++) {
        TCITEMA it = {};
        it.mask = TCIF_TEXT;
        it.pszText = (LPSTR)names[i];
        SendMessageA(g_tab, TCM_INSERTITEMA, i, (LPARAM)&it);
    }
    for (int i = 0; i < PAGE_COUNT; i++) {
        g_page[i] = CreateWindowExA(0, "PsobbvrOptionsPage", "", WS_CHILD | WS_CLIPSIBLINGS,
                                    0, 0, S(PAGE_W), S(100), wnd, nullptr, inst, nullptr);
        const int h = i == PAGE_BINDINGS ? BuildBindingsPage(g_page[i])
                    : i == PAGE_GAME ? BuildGamePage(g_page[i]) : BuildVrPage(g_page[i]);
        g_page_content[i] = S(h);
    }
    MakeButton(wnd, "Check", ID_CHECK, 0, 0, 90, 26);
    MakeButton(wnd, "Save", ID_SAVE, 0, 0, 90, 26);
    MakeButton(wnd, "Reload", ID_RELOAD, 0, 0, 100, 26);
    MakeButton(wnd, "Restore defaults", ID_DEFAULTS, 0, 0, 130, 26);
    MakeButton(wnd, "Open ini in Notepad", ID_OPENINI, 0, 0, 140, 26);
    g_log = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, S(PAGE_W), S(LOG_H),
                            wnd, nullptr, nullptr, nullptr);
    SendMessageA(g_log, WM_SETFONT, (WPARAM)g_font_mono, TRUE);
    g_status = MakeLabel(wnd, "", 0, 0, PAGE_W, 36, g_font);
    g_ui_ready = true;
}

// Moved to a monitor with a different scale: scale every control's
// position, size and font, then lay out again.
struct RescaleCtx { int from, to; HFONT old_font, old_bold, old_mono; };

BOOL CALLBACK RescaleChild(HWND h, LPARAM lp) {
    const RescaleCtx& c = *reinterpret_cast<const RescaleCtx*>(lp);
    const HWND parent = GetParent(h);
    char cls[32];
    GetClassNameA(parent, cls, sizeof(cls));
    if (_stricmp(cls, "ComboBox") == 0)
        return TRUE;   // a combo box's own edit field follows the combo
    RECT r;
    GetWindowRect(h, &r);
    int height = r.bottom - r.top;
    GetClassNameA(h, cls, sizeof(cls));
    if (_stricmp(cls, "ComboBox") == 0) {
        RECT d;   // a combo's window height is its dropped-down height
        SendMessageA(h, CB_GETDROPPEDCONTROLRECT, 0, (LPARAM)&d);
        height = d.bottom - d.top;
    }
    MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&r), 2);
    SetWindowPos(h, nullptr, MulDiv(r.left, c.to, c.from), MulDiv(r.top, c.to, c.from),
                 MulDiv(r.right - r.left, c.to, c.from), MulDiv(height, c.to, c.from),
                 SWP_NOZORDER | SWP_NOACTIVATE);
    const HFONT f = (HFONT)SendMessageA(h, WM_GETFONT, 0, 0);
    const HFONT nf = f == c.old_bold ? g_font_bold : f == c.old_mono ? g_font_mono
                   : f == c.old_font ? g_font : nullptr;
    if (nf != nullptr) SendMessageA(h, WM_SETFONT, (WPARAM)nf, TRUE);
    return TRUE;
}

void Rescale(int from_dpi) {
    for (int i = 0; i < PAGE_COUNT; i++) ScrollPageTo(i, 0);
    RescaleCtx c = {from_dpi, g_dpi, g_font, g_font_bold, g_font_mono};
    MakeFonts();
    EnumChildWindows(g_main, RescaleChild, reinterpret_cast<LPARAM>(&c));
    for (int i = 0; i < PAGE_COUNT; i++) g_page_content[i] = MulDiv(g_page_content[i], g_dpi, from_dpi);
    for (int n = 0; n < kComboNoteCount; n++)
        if (g_notes[n].tip) SendMessageA(g_notes[n].tip, TTM_SETMAXTIPWIDTH, 0, S(360));
    DeleteObject(c.old_font);
    DeleteObject(c.old_bold);
    DeleteObject(c.old_mono);
}

void OnSave() {
    switch (g_page_index) {
    case PAGE_BINDINGS: BindingsSave(); break;
    case PAGE_GAME: GameSave(); break;
    case PAGE_VR: VrSave(); break;
    }
}
void OnReload() {
    switch (g_page_index) {
    case PAGE_BINDINGS: BindingsLoad(true); break;
    case PAGE_GAME: GameLoad(true); break;
    case PAGE_VR: VrLoad(true); break;
    }
}
void OnDefaults() {
    switch (g_page_index) {
    case PAGE_BINDINGS: BindingsDefaults(); break;
    case PAGE_GAME: GameDefaultsToUi(); break;
    case PAGE_VR: VrDefaults(); break;
    }
}

LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (IsMouseDownNotify(msg, wp))
        NoteOnClick();
    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE && g_note_up >= 0)
        ShowNote(g_note_up, false);   // clicked into another program
    switch (msg) {
    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) Layout();
        return 0;
    case WM_GETMINMAXINFO:
        if (g_ui_ready) {
            // Only the height changes: from room for a few rows up to the
            // size that shows every page whole.
            MINMAXINFO* mm = reinterpret_cast<MINMAXINFO*>(lp);
            const SIZE full = FullClientSize();
            const SIZE outer = OuterSize(full.cx, full.cy);
            const SIZE least = OuterSize(full.cx, TopHeight() + S(120) + BelowHeight());
            mm->ptMinTrackSize.x = mm->ptMaxTrackSize.x = mm->ptMaxSize.x = outer.cx;
            mm->ptMinTrackSize.y = least.cy;
            mm->ptMaxTrackSize.y = outer.cy;
            if (mm->ptMaxSize.y > outer.cy) mm->ptMaxSize.y = outer.cy;
        }
        return 0;
    case 0x02E0: {  // WM_DPICHANGED: wParam = new DPI, lParam = suggested window rect
        const int from = g_dpi;
        g_dpi = HIWORD(wp);
        if (g_ui_ready && g_dpi != from) Rescale(from);
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(wnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        FitToWorkArea();   // the suggested size can overhang the new monitor
        return 0;
    }
    case WM_NOTIFY: {
        const NMHDR* h = (const NMHDR*)lp;
        if (h->hwndFrom == g_tab && h->code == TCN_SELCHANGE)
            ShowPage((int)SendMessageA(g_tab, TCM_GETCURSEL, 0, 0));
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        const int code = HIWORD(wp);
        switch (id) {
        case ID_CHECK: { Table t; BindingsCheck(t); return 0; }
        case ID_SAVE: OnSave(); return 0;
        case ID_RELOAD: OnReload(); return 0;
        case ID_DEFAULTS: OnDefaults(); return 0;
        case ID_OPENINI:
            ShellExecuteA(wnd, "open", "notepad.exe", g_ini_path, nullptr, SW_SHOWNORMAL);
            return 0;
        case ID_EXPORT: BindingsExport(); return 0;
        case ID_IMPORT: BindingsImport(); return 0;
        }
        if (id == ID_G_PRESET && code == CBN_SELCHANGE) { GameApplyPreset(Sel(g_g_preset)); return 0; }
        if (id > ID_G_PRESET && id <= ID_G_SAVEACCOUNT &&
            (code == CBN_SELCHANGE || code == BN_CLICKED)) {
            GameFieldChanged();
            return 0;
        }
        if (id >= ID_VR_FIRST && id < ID_VR_FIRST + kKnobCount) {
            // Focus on any VR control shows its explanation.
            if (code == EN_SETFOCUS || code == CBN_SETFOCUS || code == BN_SETFOCUS) {
                const Knob& k = kKnobs[id - ID_VR_FIRST];
                char line[400];
                snprintf(line, sizeof(line), "%s  (ini: %s)", k.desc,
                         k.type == K_RESOLUTION ? "width/height" : k.key);
                SetStatus(line);
            }
            return 0;
        }
        break;
    }
    case WM_MOUSEWHEEL:
        return SendMessageA(g_page[g_page_index], msg, wp, lp);
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(wnd, msg, wp, lp);
}

int g_start_page = PAGE_BINDINGS;

// Command line: [ini path] [--page bindings|game|vr]. The --page switch
// opens on that page (for shortcuts and for screenshots).
bool ResolveIniPath(const char* cmdline_in) {
    char cmdline[1024] = "";
    if (cmdline_in != nullptr) strncpy_s(cmdline, cmdline_in, sizeof(cmdline) - 1);
    if (char* pg = strstr(cmdline, "--page")) {
        const char* v = pg + 6;
        while (*v == ' ' || *v == '=') v++;
        if (_strnicmp(v, "game", 4) == 0) g_start_page = PAGE_GAME;
        else if (_strnicmp(v, "vr", 2) == 0) g_start_page = PAGE_VR;
        else g_start_page = PAGE_BINDINGS;
        *pg = '\0';   // the rest (if any) is the ini path
    }
    if (cmdline[0] != '\0') {
        const char* b = cmdline;
        const char* e = cmdline + strlen(cmdline);
        while (b < e && (*b == ' ' || *b == '\t' || *b == '"')) b++;
        while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '"' || e[-1] == '\r' || e[-1] == '\n'))
            e--;
        const size_t n = (size_t)(e - b) < MAX_PATH - 1 ? (size_t)(e - b) : MAX_PATH - 1;
        memcpy(g_ini_path, b, n);
        g_ini_path[n] = '\0';
        if (n > 0) return GetFileAttributesA(g_ini_path) != INVALID_FILE_ATTRIBUTES;
    }
    GetModuleFileNameA(nullptr, g_ini_path, MAX_PATH);
    char* slash = strrchr(g_ini_path, '\\');
    if (slash != nullptr) slash[1] = '\0';
    strcat_s(g_ini_path, "psobbvr.ini");
    return GetFileAttributesA(g_ini_path) != INVALID_FILE_ATTRIBUTES;
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR cmdline, int show) {
    EnableDpiAwareness();
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    if (!ResolveIniPath(cmdline)) {
        char msg[600];
        snprintf(msg, sizeof(msg),
                 "psobbvr.ini was not found at\n%s\n\nPut this program in the game folder "
                 "next to psobbvr.ini (or pass the ini path on the command line).", g_ini_path);
        MessageBoxA(nullptr, msg, "PSOBB VR Options", MB_ICONERROR);
        return 1;
    }
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = "PsobbvrOptions";
    RegisterClassA(&wc);
    g_main = CreateWindowExA(0, wc.lpszClassName, "PSOBB VR Options", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, nullptr, nullptr, inst, nullptr);
    // Built after creation so the layout uses the DPI of the monitor the
    // window opened on.
    g_dpi = WindowDpi(g_main);
    MakeFonts();
    BuildUi(g_main);
    FitToWorkArea();
    LoadAllPages();
    SendMessageA(g_tab, TCM_SETCURSEL, g_start_page, 0);
    ShowPage(g_start_page);
    ShowWindow(g_main, show);
    MSG m;
    while (GetMessageA(&m, nullptr, 0, 0)) {
        if (!IsDialogMessageA(g_main, &m)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
    }
    return 0;
}
