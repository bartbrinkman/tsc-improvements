#pragma once
#include <windows.h>

// [experiments] dynheap_probe=1: counts the use of the engine's 4 MB "Dynamic" heap (dynheap.cpp).
namespace tscfix::dynheap {

bool Install(HMODULE engine);
void LogUse(const char* tag);  // one line, with the other memory lines

}  // namespace tscfix::dynheap
