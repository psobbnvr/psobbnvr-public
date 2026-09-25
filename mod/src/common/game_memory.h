#pragma once
#include <cstdint>
#include <cstddef>

// Guarded reads/writes of fixed game addresses. All accesses are wrapped in
// SEH so a stale pointer (e.g. no player loaded yet) fails gracefully instead
// of crashing the game.

namespace mem {

bool ReadRaw(uintptr_t address, void* dst, size_t size);
bool WriteRaw(uintptr_t address, const void* src, size_t size);

template <typename T>
bool Read(uintptr_t address, T& out) {
    return ReadRaw(address, &out, sizeof(T));
}

template <typename T>
bool Write(uintptr_t address, const T& value) {
    return WriteRaw(address, &value, sizeof(T));
}

} // namespace mem
