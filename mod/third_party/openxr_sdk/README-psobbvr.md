# OpenXR SDK (vendored)

- Source: https://github.com/KhronosGroup/OpenXR-SDK (the pre-generated SDK
  repo), cloned 2026-08-12, API version 1.1.62. License: Apache-2.0 (see
  LICENSE).
- `include/openxr/` — headers, verbatim.
- `lib/<arch>/openxr_loader.lib` (import lib) + `bin/<arch>/openxr_loader.dll`
  — the loader built as a DLL (`-DDYNAMIC_LOADER=ON`, Release). A DLL, not
  a static lib, on purpose: the SDK's static build ignores the static-CRT
  request (its CMake predates CMP0091) and mixing it into our /MT binaries
  fails to link; and shipping a loader DLL next to the exe is exactly the
  pattern we already use for openvr_api.dll. Deploy the matching DLL next
  to whatever links it.
- The 32-bit loader resolves the runtime via
  `HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime` — that key
  exists only once SteamVR (beta 2.17.2+) has been set as the default
  OpenXR runtime from its own prompt/settings.
