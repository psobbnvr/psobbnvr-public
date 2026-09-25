#pragma once

// Minimal file logger: psobbvr-mod.log in the game's working directory,
// the previous two launches kept as .1 / .2. Every line is flushed so the
// log survives a crash.

namespace logging {

void Line(const char* format, ...);

} // namespace logging
