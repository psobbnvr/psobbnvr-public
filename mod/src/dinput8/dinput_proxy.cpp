#include "dinput8/dinput_proxy.h"

#include <cstring>
#include "common/log.h"

HRESULT STDMETHODCALLTYPE DInputProxy::CreateDevice(REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) {
    HRESULT hr = real_->CreateDevice(guid, out, outer);
    if (FAILED(hr))
        return hr;
    if (guid == GUID_SysKeyboard) {
        *out = new DInputDeviceProxy(*out, /*is_keyboard=*/true);
        logging::Line("DirectInput keyboard device wrapped");
    } else if (guid == GUID_SysMouse) {
        *out = new DInputDeviceProxy(*out, /*is_keyboard=*/false);
        logging::Line("DirectInput mouse device wrapped");
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE DInputDeviceProxy::GetDeviceState(DWORD size, LPVOID data) {
    if (is_keyboard_)
        keyinject::keyboard_polls++;  // one per game tick (see the header)
    HRESULT hr = real_->GetDeviceState(size, data);
    bool real_read_ok = SUCCEEDED(hr);
    if (FAILED(hr) && is_keyboard_) {
        // Unfocused, the read fails until something re-acquires. Try that
        // (cheap, succeeds once focus returns); if still failed and
        // synthetic keys are pending, serve them on an idle buffer.
        if (SUCCEEDED(real_->Acquire()))
            hr = real_->GetDeviceState(size, data);
        real_read_ok = SUCCEEDED(hr);
        if (FAILED(hr)) {
            bool pending = keyinject::held_ttl > 0;
            if (!keyinject::read_failing) {
                keyinject::read_failing = true;
                logging::Line("dinput: keyboard read failed (hr 0x%08lX, window "
                              "unfocused?) - %s",
                              (unsigned long)hr,
                              keyinject::unfocused_input
                                  ? "serving synthetic keys on an idle "
                                    "state until it recovers"
                                  : "unfocused_input=0, passing the failure "
                                    "through");
            }
            if (!keyinject::unfocused_input || !pending)
                return hr;
            if (data != nullptr && size > 0)
                std::memset(data, 0, size);
            hr = DI_OK;
            // Fall through to the synthetic OR below.
        }
    }
    if (FAILED(hr))
        return hr;
    if (is_keyboard_ && keyinject::read_failing && real_read_ok) {
        keyinject::read_failing = false;
        logging::Line("dinput: keyboard read recovered");
    }


    // Synthetic keys, OR'd in after the mute so they work while the overlay
    // has the keyboard. One byte per DIK scancode, high bit = down. Sources:
    // the controller mapper's held set, and the developer build's key
    // injector.
    if (is_keyboard_) {
        auto* bytes = static_cast<BYTE*>(data);
        const DWORD limit = size < 256 ? size : 256;
        const bool held_live = keyinject::held_ttl > 0;
        if (held_live)
            keyinject::held_ttl--;
        for (DWORD sc = 0; sc < limit; sc++) {
            if (held_live && keyinject::held[sc])
                bytes[sc] |= 0x80;
        }
    }
    return hr;
}
