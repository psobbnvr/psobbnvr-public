// Redirect the game's settings key. HKCU\Software\SonicTeam\PSOBB is shared
// by every PSOBB-lineage install on the machine; the registry open calls
// are hooked to rewrite the company segment, so the game uses
// HKCU\Software\psobbvr\PSOBB and other installs cannot break its settings.
#pragma once

namespace regredirect {

// Hook the four ANSI open/create functions (MinHook must be initialised).
// enabled=false ([registry] redirect=0) leaves the shared key in use.
void Install(bool enabled);

bool Active();

// Per-distinct-path redirect counts, for the process-detach log.
void LogSummary();

// The settings key our own code should use, relative to HKCU: the
// redirected key when active, the shared one otherwise.
const char* SettingsKeyPath();

// Create the redirected key and write any missing stock values
// (common/registry_seed.h), then copy the benign preferences (graphics,
// sound, font, focus sound, word wrap, save-account tick) from the shared
// key when it is newer - option.exe writes the shared key and cannot be
// redirected. No-op when the redirect is off.
void SeedOrSync();

} // namespace regredirect
