#include "common/log.h"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <share.h>
#include <windows.h>

namespace logging {

void Line(const char* format, ...) {
    static FILE* file = nullptr;
    static bool open_failed = false;
    if (!file) {
        if (open_failed)
            return;
        // Keep the previous two launches' logs; a second game instance,
        // which finds the log in use, writes a per-process log instead.
        // _SH_DENYWR keeps the log readable by other tools meanwhile.
        HANDLE probe = CreateFileA("psobbvr-mod.log", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        const bool in_use = probe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION;
        if (probe != INVALID_HANDLE_VALUE)
            CloseHandle(probe);
        if (in_use) {
            char name[64];
            sprintf_s(name, "psobbvr-mod-%lu.log", GetCurrentProcessId());
            file = _fsopen(name, "w", _SH_DENYWR);
        } else {
            MoveFileExA("psobbvr-mod.1.log", "psobbvr-mod.2.log", MOVEFILE_REPLACE_EXISTING);
            MoveFileExA("psobbvr-mod.log", "psobbvr-mod.1.log", MOVEFILE_REPLACE_EXISTING);
            file = _fsopen("psobbvr-mod.log", "w", _SH_DENYWR);
        }
        if (!file) {
            open_failed = true;
            return;
        }
    }

    time_t now = time(nullptr);
    tm local;
    localtime_s(&local, &now);
    char stamp[32];
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);
    fprintf(file, "[%s] ", stamp);

    va_list args;
    va_start(args, format);
    vfprintf(file, format, args);
    va_end(args);

    fputc('\n', file);
    fflush(file);
}

} // namespace logging
