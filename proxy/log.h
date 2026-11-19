#pragma once
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <share.h>

// Minimal thread-safe log next to the game executable (tscfix.log).
namespace tscfix {

inline CRITICAL_SECTION g_logLock;
inline FILE* g_log = nullptr;

// The log is written anew at each launch. If the last session's log holds a crash record, it is
// kept first: copied to %LOCALAPPDATA%\tscfix\crashes\crash-<date>-<time>.log (outside the game
// folder), so a crash can be read later from its record.
inline void KeepCrashLog(const char* path) {
    FILE* f = _fsopen(path, "rb", _SH_DENYNO);
    if (!f) return;
    bool crashed = false;
    char line[1024];
    while (!crashed && fgets(line, sizeof line, f)) crashed = strstr(line, "] crash: exception") != nullptr;
    fclose(f);
    if (!crashed) return;
    char dir[MAX_PATH], dest[MAX_PATH];
    if (!GetEnvironmentVariableA("LOCALAPPDATA", dir, MAX_PATH - 48)) return;
    strcat_s(dir, "\\tscfix");
    CreateDirectoryA(dir, nullptr);
    strcat_s(dir, "\\crashes");
    CreateDirectoryA(dir, nullptr);
    WIN32_FILE_ATTRIBUTE_DATA info{};
    SYSTEMTIME utc{}, t{};
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &info) && FileTimeToSystemTime(&info.ftLastWriteTime, &utc))
        SystemTimeToTzSpecificLocalTime(nullptr, &utc, &t);
    sprintf_s(dest, "%s\\crash-%04d%02d%02d-%02d%02d%02d.log", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    CopyFileA(path, dest, TRUE);
}

inline void LogOpen() {
    InitializeCriticalSection(&g_logLock);
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    strcpy_s(slash ? slash + 1 : path, MAX_PATH - (slash ? slash + 1 - path : 0), "tscfix.log");
    KeepCrashLog(path);
    g_log = _fsopen(path, "w", _SH_DENYWR);  // others may read it while the game runs
}

inline void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logLock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d [%5lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds,
            GetCurrentThreadId());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logLock);
}

}  // namespace tscfix
