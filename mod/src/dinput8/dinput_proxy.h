#pragma once
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

// Forwarding wrappers around the game's DirectInput objects. They OR
// synthetic key presses into the keyboard state, so the game's own input
// pipeline (keymap, menus, palette) sees real presses, and in the developer
// build mute the polled state while the ImGui panel wants the mouse or
// keyboard. The game's pad record cannot be injected into: its button masks
// are a mirror and the action dispatch reads key state elsewhere.

namespace keyinject {
// The controller mapper's held-key set, replaced each frame via
// PsobbvrSetSyntheticKeys. held_ttl counts down per read so a stalled
// pusher releases every key within a fraction of a second.
inline unsigned char held[256] = {};
inline int held_ttl = 0;
// [vr] unfocused_input: the game acquires the keyboard at the foreground
// level, so its read fails while the window is unfocused. When synthetic
// keys are pending, serve an idle state with them OR'd in instead, so
// controller buttons work while focus is elsewhere (Steam Link, the
// SteamVR dashboard). With nothing pending the failure passes through;
// the real keyboard is never served unfocused.
inline bool unfocused_input = true;
inline bool read_failing = false;  // one log line per failure episode
// Keyboard polls = game ticks (one read per frame, success or failure).
// Exported as PsobbvrKeyboardPolls for the d3d8 side's swing holds, which
// pause the animation clock and so cannot count ticks from it.
inline unsigned long keyboard_polls = 0;
}

class DInputDeviceProxy final : public IDirectInputDevice8A {
public:
    DInputDeviceProxy(IDirectInputDevice8A* real, bool is_keyboard)
        : real_(real), is_keyboard_(is_keyboard) {}

    // Implemented in the .cpp: muting and key injection.
    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD size, LPVOID data) override;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* out) override { return real_->QueryInterface(riid, out); }
    ULONG STDMETHODCALLTYPE AddRef() override { return real_->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return real_->Release(); }

    // IDirectInputDevice8A pass-through
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS caps) override { return real_->GetCapabilities(caps); }
    HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA callback, LPVOID ref, DWORD flags) override { return real_->EnumObjects(callback, ref, flags); }
    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID prop, LPDIPROPHEADER header) override { return real_->GetProperty(prop, header); }
    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID prop, LPCDIPROPHEADER header) override { return real_->SetProperty(prop, header); }
    HRESULT STDMETHODCALLTYPE Acquire() override { return real_->Acquire(); }
    HRESULT STDMETHODCALLTYPE Unacquire() override { return real_->Unacquire(); }
    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags) override { return real_->GetDeviceData(size, data, count, flags); }
    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT format) override { return real_->SetDataFormat(format); }
    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE event) override { return real_->SetEventNotification(event); }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND window, DWORD flags) override { return real_->SetCooperativeLevel(window, flags); }
    HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA info, DWORD obj, DWORD how) override { return real_->GetObjectInfo(info, obj, how); }
    HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEA info) override { return real_->GetDeviceInfo(info); }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND window, DWORD flags) override { return real_->RunControlPanel(window, flags); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE inst, DWORD version, REFGUID guid) override { return real_->Initialize(inst, version, guid); }
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID guid, LPCDIEFFECT effect, LPDIRECTINPUTEFFECT* out, LPUNKNOWN outer) override { return real_->CreateEffect(guid, effect, out, outer); }
    HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKA callback, LPVOID ref, DWORD type) override { return real_->EnumEffects(callback, ref, type); }
    HRESULT STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOA info, REFGUID guid) override { return real_->GetEffectInfo(info, guid); }
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD state) override { return real_->GetForceFeedbackState(state); }
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD command) override { return real_->SendForceFeedbackCommand(command); }
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK callback, LPVOID ref, DWORD flags) override { return real_->EnumCreatedEffectObjects(callback, ref, flags); }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE escape) override { return real_->Escape(escape); }
    HRESULT STDMETHODCALLTYPE Poll() override { return real_->Poll(); }
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD size, LPCDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags) override { return real_->SendDeviceData(size, data, count, flags); }
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(LPCSTR file, LPDIENUMEFFECTSINFILECALLBACK callback, LPVOID ref, DWORD flags) override { return real_->EnumEffectsInFile(file, callback, ref, flags); }
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(LPCSTR file, DWORD count, LPDIFILEEFFECT effects, DWORD flags) override { return real_->WriteEffectToFile(file, count, effects, flags); }
    HRESULT STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATA format, LPCSTR user, DWORD flags) override { return real_->BuildActionMap(format, user, flags); }
    HRESULT STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATA format, LPCSTR user, DWORD flags) override { return real_->SetActionMap(format, user, flags); }
    HRESULT STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA header) override { return real_->GetImageInfo(header); }

private:
    IDirectInputDevice8A* real_;
    bool is_keyboard_;
};

class DInputProxy final : public IDirectInput8A {
public:
    explicit DInputProxy(IDirectInput8A* real) : real_(real) {}

    // Implemented in the .cpp — wraps the keyboard/mouse devices it creates.
    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) override;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* out) override { return real_->QueryInterface(riid, out); }
    ULONG STDMETHODCALLTYPE AddRef() override { return real_->AddRef(); }
    ULONG STDMETHODCALLTYPE Release() override { return real_->Release(); }

    // IDirectInput8A pass-through
    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD type, LPDIENUMDEVICESCALLBACKA callback, LPVOID ref, DWORD flags) override { return real_->EnumDevices(type, callback, ref, flags); }
    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID guid) override { return real_->GetDeviceStatus(guid); }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND window, DWORD flags) override { return real_->RunControlPanel(window, flags); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE inst, DWORD version) override { return real_->Initialize(inst, version); }
    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID guid, LPCSTR name, LPGUID out) override { return real_->FindDevice(guid, name, out); }
    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCSTR user, LPDIACTIONFORMATA format, LPDIENUMDEVICESBYSEMANTICSCBA callback, LPVOID ref, DWORD flags) override { return real_->EnumDevicesBySemantics(user, format, callback, ref, flags); }
    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK callback, LPDICONFIGUREDEVICESPARAMSA params, DWORD flags, LPVOID ref) override { return real_->ConfigureDevices(callback, params, flags, ref); }

private:
    IDirectInput8A* real_;
};
