#pragma once

// Frame probe: with PSOBBVR_PROBE=1 in the environment, logs draw calls,
// vertex formats, viewport / render-target changes and transform sets to
// psobbvr-d3d8-probe.log in the working directory. Off otherwise.
// Developer build only; the release build keeps empty stubs so the call
// sites compile away. current_fvf is real state in both builds.

#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <windows.h>

namespace probe {

inline DWORD current_fvf = 0;  // the FVF last set by the game


constexpr bool enabled = false;
inline void Init() {}
inline void ArmFrames(int) {}
// Templates rather than C varargs: MSVC never inlines a varargs function,
// which would keep every call and its format string in the binary.
template <typename... Args>
inline void Log(const char*, Args...) {}
template <typename... Args>
inline void LogRaw(const char*, Args...) {}
inline void OnPresent() {}


} // namespace probe
