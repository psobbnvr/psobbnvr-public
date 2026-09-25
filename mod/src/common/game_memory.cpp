#include "common/game_memory.h"

#include <cstring>
#include <windows.h>

namespace mem {

bool ReadRaw(uintptr_t address, void* dst, size_t size) {
    __try {
        std::memcpy(dst, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteRaw(uintptr_t address, const void* src, size_t size) {
    __try {
        std::memcpy(reinterpret_cast<void*>(address), src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace mem
