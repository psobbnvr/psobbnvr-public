#pragma once
#include <windows.h>
#include "d3d8.hpp"  // d3d8to9's D3D8 declarations

// Game window setup, installed once the game has created its D3D8 device:
// sizes the window to the backbuffer and subclasses it for the recenter
// key. The developer build adds the ImGui debug panel, drawn from the
// device proxy's Present hook.

namespace overlay {

// Called from the InitD3D hook after the game's device exists. Subclasses
// the game window and matches its size to the backbuffer (in the developer
// build also wraps the game's device-pointer global and sets up ImGui).
// The game runs its D3D init more than once at startup (it re-creates the
// device), so this must handle being called again: it moves over to the
// new device.
void Install();


} // namespace overlay
