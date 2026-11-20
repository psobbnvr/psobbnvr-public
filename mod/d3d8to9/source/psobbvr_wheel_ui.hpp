#pragma once

// The spell wheel's shared view state: what the input side
// (psobbvr_wheel_input.hpp, run inside controller::OnFrame) decided this
// frame, and what the drawing side (psobbvr_wheel_draw.hpp, run after it
// and at the hands' draw point) shows. One writer, one reader, both on the
// render thread; plain variables are enough.
//
// The armed choice itself lives in psobbvr_wheel.hpp (wheel::Choice()):
// the input side arms it on a release, the game side casts it.

#include <cstdint>

#include "psobbvr_wheel_core.hpp"

namespace wheelui {

struct State {
    bool open = false;                 // the ring is up
    unsigned open_serial = 0;          // +1 at each open (the draw side re-captures its pose)
    int cls = -1;                      // the local player's class byte (0..11), -1 unknown
    int page = 0;                      // the page shown, 0-based
    int page_count = 0;                // the class's pages; 0 = none set up (the empty ring + its message)
    int highlight = -1;                // the lit segment 0..7 (N, NE, .. NW), -1 none (the stick centred)
    wheelcore::Entry segments[wheelcore::kSegments] = {};  // the page's entries (Kind::None = empty)
    unsigned pick_serial = 0;          // +1 at each highlight change (a redraw)
    unsigned fire_serial = 0;          // +1 at each bare-swing fire request posted (the hand icon's flash)
};

inline State state;

}  // namespace wheelui
