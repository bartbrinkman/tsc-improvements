#include "status.h"

#include <windows.h>

#include <map>
#include <string>

namespace tscfix::status {
namespace {

std::map<std::string, std::string> g_fields;
std::wstring g_baseTitle;
HWND g_wnd = nullptr;

BOOL CALLBACK FindGameWindow(HWND hwnd, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    char cls[64];
    if (pid == GetCurrentProcessId() && GetClassNameA(hwnd, cls, sizeof cls) && strcmp(cls, "RailWorks") == 0) {
        g_wnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

void Apply() {
    if (!g_wnd) {
        EnumWindows(FindGameWindow, 0);
        if (!g_wnd) return;
        wchar_t title[256];
        GetWindowTextW(g_wnd, title, 256);
        g_baseTitle = title;
    }
    std::wstring text = g_baseTitle;
    if (!g_fields.empty()) {
        text += L"  —  tscfix:";
        bool first = true;
        for (auto& [k, v] : g_fields) {
            text += first ? L" " : L"  |  ";
            first = false;
            text += std::wstring(v.begin(), v.end());
        }
    }
    SetWindowTextW(g_wnd, text.c_str());
}

}  // namespace

void Set(const char* field, const char* text) {
    if (text && *text) g_fields[field] = text;
    else g_fields.erase(field);
    Apply();
}

}  // namespace tscfix::status
