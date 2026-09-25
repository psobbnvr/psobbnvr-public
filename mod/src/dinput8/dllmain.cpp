// dinput8.dll proxy. The game imports dinput8.dll, so this loads before
// any game code runs. It redirects the game's registry key
// (registry_redirect.cpp), hooks the game's D3D init to set up the game
// window and, in the developer build, the ImGui panel (overlay.cpp), and
// wraps DirectInput so the controller mapper's synthetic keys reach the
// game.

#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <cstring>

#include "MinHook.h"
#include "common/log.h"
#include "common/psobb_addresses.h"
#include "dinput8/dinput_proxy.h"
#include "dinput8/overlay.h"
#include "dinput8/registry_redirect.h"

namespace {

using DirectInput8CreateFn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
DirectInput8CreateFn g_real_directinput8_create = nullptr;

bool EnsureRealDInput8Loaded() {
    if (g_real_directinput8_create)
        return true;
    char path[MAX_PATH];
    if (!GetSystemDirectoryA(path, MAX_PATH))
        return false;
    strcat_s(path, "\\dinput8.dll");
    HMODULE real = LoadLibraryA(path);
    if (!real) {
        logging::Line("failed to load system dinput8.dll");
        return false;
    }
    g_real_directinput8_create = (DirectInput8CreateFn)GetProcAddress(real, "DirectInput8Create");
    if (!g_real_directinput8_create) {
        logging::Line("system dinput8.dll has no DirectInput8Create?");
        return false;
    }
    return true;
}

// Forces WINDOW_MODE and two GRAPHICCTRL fields in the game's key
// (regredirect::SettingsKeyPath()) before any game code reads them; the
// redirect off, or preferences copied from the shared key, can bring back
// bad values:
//   WINDOW_MODE: must be 1 (windowed); 0 (fullscreen) fails device
//     creation and the game exits silently.
//   GRAPHICCTRL byte 8 (dword 2): shadow detail, 0 = Low (blob), 2 = High.
//     High renders silhouette shadows from camera state we overwrite with
//     the head pose, so they swim in VR. Other fields are left alone.
void ForceSharedRegistrySettings() {
    HKEY key = nullptr;
    const char* key_path = regredirect::SettingsKeyPath();
    LSTATUS status = RegCreateKeyExA(HKEY_CURRENT_USER, key_path, 0, nullptr,
                                     0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        logging::Line("registry: cannot open HKCU\\%s (error %ld)", key_path, (long)status);
        return;
    }

    // --- WINDOW_MODE: force 1 (windowed) ---
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    status = RegQueryValueExA(key, "WINDOW_MODE", nullptr, &type,
                              reinterpret_cast<BYTE*>(&value), &size);
    const bool present = (status == ERROR_SUCCESS && type == REG_DWORD);
    if (!present || value != 1) {
        DWORD windowed = 1;
        status = RegSetValueExA(key, "WINDOW_MODE", 0, REG_DWORD,
                                reinterpret_cast<const BYTE*>(&windowed), sizeof(windowed));
        if (status != ERROR_SUCCESS)
            logging::Line("registry: failed to set WINDOW_MODE=1 (error %ld)", (long)status);
        else if (present)
            logging::Line("registry: WINDOW_MODE was %lu, forced to 1%s", (unsigned long)value,
                          regredirect::Active() ? "" : " (shared key - another PSOBB install reset it)");
        else
            logging::Line("registry: WINDOW_MODE was unset, forced to 1");
    }

    // --- GRAPHICCTRL: shadow (byte 8) and "Advanced Effect" (byte 4) to 0 ---
    // Only those two bytes are patched. Advanced effects put grass and
    // particles on the screen plane, where the 2D classifier mistakes them
    // for HUD and the HUD scales up huge. A missing or short blob is
    // skipped (the game then defaults shadows off).
    constexpr DWORD kShadowByteOffset = 8;
    constexpr DWORD kAdvancedByteOffset = 4;
    BYTE blob[64] = {};
    DWORD blob_size = sizeof(blob);
    DWORD blob_type = 0;
    status = RegQueryValueExA(key, "GRAPHICCTRL", nullptr, &blob_type, blob, &blob_size);
    if (status == ERROR_SUCCESS && blob_type == REG_BINARY && blob_size >= 36) {
        // Log the blob as read, before the forcing below.
        DWORD g[9];
        memcpy(g, blob, sizeof(g));
        logging::Line("registry: GRAPHICCTRL as read: tier=%lu advanced=%lu shadow=%lu enemy=%lu map=%lu "
                      "clip=%lu fog=%lu lowres=%lu frameskip=%lu",
                      g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8]);
    }
    if (status == ERROR_SUCCESS && blob_type == REG_BINARY && blob_size > kShadowByteOffset) {
        const BYTE shadow_was = blob[kShadowByteOffset];
        const BYTE advanced_was = blob[kAdvancedByteOffset];
        if (shadow_was != 0 || advanced_was != 0) {
            blob[kShadowByteOffset] = 0;
            blob[kAdvancedByteOffset] = 0;
            status = RegSetValueExA(key, "GRAPHICCTRL", 0, REG_BINARY, blob, blob_size);
            if (status != ERROR_SUCCESS)
                logging::Line("registry: failed to set GRAPHICCTRL shadow=Low / advanced=off (error %ld)", (long)status);
            if (status == ERROR_SUCCESS && shadow_was != 0)
                logging::Line("registry: GRAPHICCTRL shadow detail was %u, forced to 0 (Low - detailed shadows swim in VR)",
                              (unsigned)shadow_was);
            if (status == ERROR_SUCCESS && advanced_was != 0)
                logging::Line("registry: GRAPHICCTRL advanced effects was %u, forced to 0 (their screen-space particles break the HUD placement in VR)",
                              (unsigned)advanced_was);
        }
    }

    // Log what the game will see. A non-zero CTRLBUF is not fixed here and
    // makes the resolution come out wrong; an absent one (the seed writes
    // it) makes the game's D3D init fail.
    {
        DWORD wm = 0xFFFFFFFF, wm_size = sizeof(wm), type = 0;
        RegQueryValueExA(key, "WINDOW_MODE", nullptr, &type,
                         reinterpret_cast<BYTE*>(&wm), &wm_size);
        BYTE blob[256] = {};
        DWORD blob_size = sizeof(blob);
        int shadow = -1;
        if (RegQueryValueExA(key, "GRAPHICCTRL", nullptr, &type, blob, &blob_size) ==
                ERROR_SUCCESS && blob_size > 8)
            shadow = blob[8];
        BYTE ctrl[64] = {};
        DWORD ctrl_size = sizeof(ctrl);
        const char* ctrl_state = "absent";
        if (RegQueryValueExA(key, "CTRLBUF", nullptr, &type, ctrl, &ctrl_size) ==
            ERROR_SUCCESS) {
            ctrl_state = "zero";
            for (DWORD i = 0; i < ctrl_size; i++)
                if (ctrl[i] != 0) { ctrl_state = "NON-ZERO"; break; }
        }
        logging::Line("registry: read-back HKCU\\%s WINDOW_MODE=%lu shadow=%d CTRLBUF=%s%s",
                      key_path, (unsigned long)wm, shadow, ctrl_state,
                      strcmp(ctrl_state, "NON-ZERO") == 0
                          ? " (WARNING: resolution may be wrong - zero CTRLBUF "
                            "with the game closed)"
                          : "");
    }
    RegCloseKey(key);
}

// The game's InitD3D takes ecx and edx (a small integer, not an object
// pointer) and returns a status in eax that the caller checks, so the hook
// must forward it or the game exits.
using GameInitD3DFn = uint32_t(__fastcall*)(void* ecx_arg, void* edx_arg);
GameInitD3DFn g_original_init_d3d = nullptr;

// The game retries a failed InitD3D once, then exits with code 1 and no
// message. The second failure shows a message box instead (skipped when
// PSOBBVR_QUIET is set).
void ReportInitD3DFailure() {
    static int failures = 0;
    failures++;
    logging::Line("game InitD3D failed (%d of the game's 2 attempts)", failures);
    if (failures < 2 || GetEnvironmentVariableA("PSOBBVR_QUIET", nullptr, 0) != 0)
        return;
    MessageBoxA(nullptr,
                "The game's own Direct3D setup failed twice, so the game is closing.\n\n"
                "Please send psobbvr-mod.log and psobbvr-vr.log from the "
                "game folder, and say which graphics card and Windows version you have.",
                "PSOBB VR - the game could not start",
                MB_ICONERROR | MB_OK | MB_TOPMOST | MB_SETFOREGROUND);
}

uint32_t __fastcall HookedGameInitD3D(void* ecx_arg, void* edx_arg) {
    logging::Line("game InitD3D called (ecx=%p, edx=%p)", ecx_arg, edx_arg);
    uint32_t ret = g_original_init_d3d(ecx_arg, edx_arg);
    logging::Line("game InitD3D returned 0x%X, (re)installing overlay", ret);
    if (ret == 0)
        ReportInitD3DFailure();
    overlay::Install();
    return ret;
}

// An integer from psobbvr.ini next to the game exe (def when unreadable).
int IniInt(const char* section, const char* key, int def) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return def;
    char* slash = strrchr(path, '\\');
    if (slash == nullptr) return def;
    *slash = '\0';
    strcat_s(path, "\\psobbvr.ini");
    return GetPrivateProfileIntA(section, key, def, path);
}

// [registry] redirect=1 (default).
bool RedirectEnabledFromIni() {
    return IniInt("registry", "redirect", 1) != 0;
}

void Initialize() {
#ifndef PSOBBVR_GIT_REV
#define PSOBBVR_GIT_REV "unknown"
#endif
    logging::Line("---- psobbvr dinput8 proxy loaded (build %s, compiled %s %s) ----",
                  PSOBBVR_GIT_REV, __DATE__, __TIME__);
    // Controller buttons while the window is unfocused (dinput_proxy.h).
    keyinject::unfocused_input = IniInt("vr", "unfocused_input", 1) != 0;
    logging::Line("dinput: unfocused_input=%d (synthetic keys served while the "
                  "keyboard read fails unfocused)",
                  keyinject::unfocused_input ? 1 : 0);

    if (GetEnvironmentVariableA("PSOBBVR_NO_HOOK", nullptr, 0)) {
        logging::Line("PSOBBVR_NO_HOOK set, not touching the game at all (pure DLL forwarding)");
        return;
    }

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        logging::Line("MH_Initialize failed: %s", MH_StatusToString(status));
        ForceSharedRegistrySettings();   // still self-heal, on the shared key
        return;
    }
    // The registry hooks go in first so every later registry access - the
    // seed, the self-heal, and all of the game's - lands in our own key.
    regredirect::Install(RedirectEnabledFromIni());
    regredirect::SeedOrSync();
    ForceSharedRegistrySettings();
    status = MH_CreateHook(reinterpret_cast<LPVOID>(psobb::kInitD3DFunc),
                           reinterpret_cast<LPVOID>(&HookedGameInitD3D),
                           reinterpret_cast<LPVOID*>(&g_original_init_d3d));
    if (status != MH_OK) {
        logging::Line("MH_CreateHook(InitD3D) failed: %s", MH_StatusToString(status));
        return;
    }
    status = MH_EnableHook(reinterpret_cast<LPVOID>(psobb::kInitD3DFunc));
    if (status != MH_OK) {
        logging::Line("MH_EnableHook(InitD3D) failed: %s", MH_StatusToString(status));
        return;
    }
    logging::Line("InitD3D hook installed at 0x%08X", (unsigned)psobb::kInitD3DFunc);
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        Initialize();
    } else if (reason == DLL_PROCESS_DETACH) {
        logging::Line("process detach (normal exit path reached)");
        regredirect::LogSummary();
        MH_Uninitialize();
    }
    return TRUE;
}


// Keyboard polls so far = game ticks (see keyinject::keyboard_polls).
extern "C" __declspec(dllexport) unsigned long PsobbvrKeyboardPolls() {
    return keyinject::keyboard_polls;
}

// Replaces the controller mapper's held-key set: up to `count` DIK codes
// (nullptr/0 = none). Held until replaced or the TTL runs out
// (dinput_proxy.h), so callers push every frame.
extern "C" __declspec(dllexport) void PsobbvrSetSyntheticKeys(
    const unsigned char* scancodes, int count) {
    memset(keyinject::held, 0, sizeof(keyinject::held));
    if (scancodes != nullptr) {
        for (int i = 0; i < count && i < 32; i++)
            keyinject::held[scancodes[i]] = 1;
    }
    keyinject::held_ttl = count > 0 ? 10 : 0;
}

HRESULT WINAPI DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    if (!EnsureRealDInput8Loaded())
        return DIERR_GENERIC;

    HRESULT hr = g_real_directinput8_create(inst, version, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(riid, IID_IDirectInput8A)) {
        *out = new DInputProxy(static_cast<IDirectInput8A*>(*out));
        logging::Line("DirectInput8 (ANSI) interface wrapped");
    } else if (SUCCEEDED(hr)) {
        logging::Line("DirectInput8Create for non-ANSI interface, passing through unwrapped");
    }
    return hr;
}
