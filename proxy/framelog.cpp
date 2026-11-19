#include "framelog.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace tscfix::framelog {
namespace {

HANDLE g_evStart = nullptr, g_evStop = nullptr, g_evDone = nullptr;
bool g_running = false;
LARGE_INTEGER g_last{};
std::vector<float> g_ms;

HANDLE Event(const wchar_t* which) {
    wchar_t name[96];
    swprintf_s(name, L"Local\\tscfix.frames.%s.%lu", which, GetCurrentProcessId());
    return CreateEventW(nullptr, TRUE, FALSE, name);
}

void Write() {
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "tscfix-frames.csv");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") || !f) return;
    fprintf(f, "Application,MsBetweenPresents\n");
    for (float ms : g_ms) fprintf(f, "RailWorks64.exe,%.3f\n", ms);
    fclose(f);
}

}  // namespace

void Init() {
    if (g_evStart) return;
    g_evStart = Event(L"start");
    g_evStop = Event(L"stop");
    g_evDone = Event(L"done");
    g_ms.reserve(1 << 16);
}

void OnFrame() {
    if (!g_evStart) return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_running) {
        if (WaitForSingleObject(g_evStart, 0) != WAIT_OBJECT_0) return;
        ResetEvent(g_evStart);
        ResetEvent(g_evDone);
        g_ms.clear();
        g_running = true;
    } else {
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        g_ms.push_back(float(double(now.QuadPart - g_last.QuadPart) * 1000.0 / freq.QuadPart));
        if (WaitForSingleObject(g_evStop, 0) == WAIT_OBJECT_0) {
            ResetEvent(g_evStop);
            g_running = false;
            Write();
            SetEvent(g_evDone);
        }
    }
    g_last = now;
}

}  // namespace tscfix::framelog
