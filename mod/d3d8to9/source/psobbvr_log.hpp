#pragma once

// The d3d8 side's log (psobbvr-vr.log in the game folder) and a guarded
// memory check.
//
// Logging follows [debug] enabled in psobbvr.ini (default on), read at the
// first line. The previous two logs are kept as psobbvr-vr.1.log and
// psobbvr-vr.2.log; a second game instance writes psobbvr-vr-<pid>.log.

#include <cstdarg>
#include <cstdio>
#include <share.h>
#include <windows.h>

namespace diag {

inline bool enabled = true;
inline FILE* log_file = nullptr;
inline bool log_opened = false;

// Read [debug] enabled, rotate the old logs, open the new one.
inline void OpenLog() {
    log_opened = true;
    char ini[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, ini, MAX_PATH);
    char* slash = n ? strrchr(ini, '\\') : nullptr;
    if (slash != nullptr) {
        slash[1] = '\0';
        strcat_s(ini, "psobbvr.ini");
        enabled = GetPrivateProfileIntA("debug", "enabled", 1, ini) != 0;
    }
    if (!enabled)
        return;
    const char* kLog = "psobbvr-vr.log";
    HANDLE probe = CreateFileA(kLog, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    const bool in_use = probe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION;
    if (probe != INVALID_HANDLE_VALUE)
        CloseHandle(probe);
    if (in_use) {
        char name[64];
        sprintf_s(name, "psobbvr-vr-%lu.log", GetCurrentProcessId());
        log_file = _fsopen(name, "w", _SH_DENYWR);
    } else {
        MoveFileExA("psobbvr-vr.1.log", "psobbvr-vr.2.log", MOVEFILE_REPLACE_EXISTING);
        MoveFileExA(kLog, "psobbvr-vr.1.log", MOVEFILE_REPLACE_EXISTING);
        // _SH_DENYWR: readable by other tools while the game runs.
        log_file = _fsopen(kLog, "w", _SH_DENYWR);
    }
    if (log_file != nullptr) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fprintf(log_file, "---- psobbvr d3d8 (build %s), %04u-%02u-%02u %02u:%02u:%02u ----\n",
                PSOBBVR_GIT_REV, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        fflush(log_file);
    }
}

inline void Log(const char* format, ...) {
    if (!log_opened)
        OpenLog();
    if (log_file == nullptr)
        return;
    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    fputc('\n', log_file);
    fflush(log_file);
}

// True when [addr, addr+len) is committed memory in one region with the
// needed access. Guards reads and writes of game structures.
inline bool Accessible(uintptr_t addr, size_t len, bool for_write) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0)
        return false;
    if (mbi.State != MEM_COMMIT)
        return false;
    const DWORD readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY |
                           PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & (for_write ? writable : readable)) == 0)
        return false;
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return addr + len <= region_end;
}

// Diagnostic state shared with the developer tools build: trace budgets
// and requests the tools arm (idle in normal builds), and state and
// counters they print.
inline int rhw_census_frames = 0;
inline int move_trace_frames = 0;
inline int mag_trace = 0;
inline bool mag_posed_root_valid = false;
inline unsigned mag_root_synced = 0;
inline void* mag_object = nullptr;
inline int trail_trace = 0;
inline bool trail_installed = false;
inline int trail_install_status = -1;
inline unsigned trail_hook_calls = 0;
inline unsigned trail_last_quads = 0;
inline LONGLONG hit_census_until_qpc = 0;
inline bool HitCensusArmed() {
    if (hit_census_until_qpc == 0)
        return false;
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return n.QuadPart < hit_census_until_qpc;
}
inline bool gun_ray_relog_request = false;
inline bool bindings_dump = false;
inline bool bindings_reload = false;
inline unsigned bodycull_applied = 0;
inline int menutrace_frames = 0;
inline bool menutrace_full = false;
inline bool ui_focused = false;
inline unsigned menutrace_frame = 0;
inline int menutrace_draw = 0;
inline const void* current_texture0 = nullptr;

}  // namespace diag
