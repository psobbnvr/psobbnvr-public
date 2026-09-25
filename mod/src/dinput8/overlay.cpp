#include "dinput8/overlay.h"

#include <cstdint>
#include <cstdio>

#include "common/game_memory.h"
#include "common/log.h"
#include "common/psobb_addresses.h"

namespace overlay {
namespace {

bool g_installed = false;
HWND g_window = nullptr;
WNDPROC g_game_wndproc = nullptr;
IDirect3DDevice8* g_real_device = nullptr;

// Exports of our d3d8.dll (null with a stock d3d8.dll).
void (WINAPI* g_vr_recenter)() = nullptr;

// The backbuffer size: the configured game resolution, however the window
// is sized. The window is matched to it; the ImGui panel lays out in it,
// with mouse positions scaled from window coordinates.
int g_display_width = 0;
int g_display_height = 0;


// Backslash = recenter. The numpad belongs to the in-headset hotkeys
// (psobbvr_hotkeys.hpp).
bool IsRecenterKey(WPARAM wparam, LPARAM lparam) {
    return wparam == VK_OEM_5 && !(lparam & 0x40000000);
}

void RecenterFromKey() {
    if (g_vr_recenter) {
        g_vr_recenter();
        logging::Line("overlay: recenter hotkey (backslash)");
    } else {
        logging::Line("overlay: recenter hotkey pressed but PsobbvrVrRecenter export missing");
    }
}

LRESULT CALLBACK OverlayWndProc(HWND wnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && IsRecenterKey(wparam, lparam)) {
        RecenterFromKey();
        return 0;
    }
    return CallWindowProcA(g_game_wndproc, wnd, msg, wparam, lparam);
}


// Resize the game window; D3D8 stretches the 640x480 backbuffer to fill it,
// and our mouse handling already scales window coords back to backbuffer
// coords, so nothing else needs to know.
void ResizeGameWindow(float scale) {
    RECT rect = { 0, 0, (LONG)(g_display_width * scale), (LONG)(g_display_height * scale) };
    DWORD style = (DWORD)GetWindowLongPtrA(g_window, GWL_STYLE);
    DWORD ex_style = (DWORD)GetWindowLongPtrA(g_window, GWL_EXSTYLE);
    AdjustWindowRectEx(&rect, style, FALSE, ex_style);
    SetWindowPos(g_window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}


bool QueryBackbufferSize(IDirect3DDevice8* device) {
    IDirect3DSurface8* backbuffer = nullptr;
    if (FAILED(device->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)) || !backbuffer)
        return false;
    D3DSURFACE_DESC8 desc;
    bool ok = SUCCEEDED(backbuffer->GetDesc(&desc));
    backbuffer->Release();
    if (ok) {
        g_display_width = (int)desc.Width;
        g_display_height = (int)desc.Height;
    }
    return ok;
}

} // namespace

void Install() {
    // Debug kill switches for bisecting startup problems.
    if (GetEnvironmentVariableA("PSOBBVR_NO_INSTALL", nullptr, 0)) {
        logging::Line("overlay: PSOBBVR_NO_INSTALL set, skipping install entirely");
        return;
    }
    const bool skip_wndproc = GetEnvironmentVariableA("PSOBBVR_NO_WNDPROC", nullptr, 0) != 0;

    IDirect3DDevice8* device = nullptr;
    if (!mem::Read(psobb::kDevicePointer, device) || !device) {
        logging::Line("overlay: device pointer global is empty, cannot install");
        return;
    }
    HWND window = nullptr;
    if (!mem::Read(psobb::kWindowHandle, window) || !window) {
        logging::Line("overlay: window handle global is empty, cannot install");
        return;
    }

    if (g_installed && device == g_real_device) {
        logging::Line("overlay: InitD3D ran again but the device is unchanged");
        return;
    }

    if (g_installed)
        logging::Line("overlay: game re-created its device (old %p -> new %p), re-installing",
                      (void*)g_real_device, (void*)device);

    g_real_device = device;

    HMODULE d3d8 = GetModuleHandleA("d3d8.dll");
    if (d3d8)
        g_vr_recenter = reinterpret_cast<void (WINAPI*)()>(
            GetProcAddress(d3d8, "PsobbvrVrRecenter"));
    logging::Line("overlay: d3d8.dll exports: vr-recenter %s",
                  g_vr_recenter ? "found" : "absent");

    if (!QueryBackbufferSize(device)) {
        logging::Line("overlay: failed to query backbuffer size");
        return;
    }


    // Subclass the game window (again, if the window was re-created too).
    if (skip_wndproc) {
        logging::Line("overlay: PSOBBVR_NO_WNDPROC set, leaving the window procedure untouched");
    } else if (window != g_window) {
        g_window = window;
        g_game_wndproc = (WNDPROC)GetWindowLongPtrA(g_window, GWLP_WNDPROC);
        SetWindowLongPtrA(g_window, GWLP_WNDPROC, (LONG_PTR)OverlayWndProc);
    }

    // The game sizes its window for 640x480; match it to the (overridden)
    // backbuffer for an unstretched image.
    RECT client;
    if (GetClientRect(g_window, &client) &&
        (client.right != g_display_width || client.bottom != g_display_height)) {
        ResizeGameWindow(1.0f);
        logging::Line("overlay: resized game window to match %dx%d backbuffer",
                      g_display_width, g_display_height);
    }

    g_installed = true;
    logging::Line("overlay: installed (backbuffer %dx%d, device %p, hwnd %p)",
                  g_display_width, g_display_height, (void*)device, (void*)window);
}


} // namespace overlay
